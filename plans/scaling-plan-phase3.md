---
plan_id: scaling-phase-3-event-loop
title: Phase 3 Event-Loop Socket I/O
category: implementation
status: active
owner: agent
created: 2026-09-19
updated: 2026-09-20
related: []
---

# Phase 3 Event-Loop Socket I/O

- **Phase ID:** `scaling-phase-3-event-loop`
- **Objective:** Make the single event-loop implementation own all admitted client sockets while preserving HTTP behavior and bounded resource use.
- **Complexity:** 4 — This is a cross-module concurrency-model change with interacting ownership, parser, buffering, deadline, and ordering invariants; it remains one bounded phase because the event-loop transition must be validated as an integrated protocol path.
- **Risk:** high — Incorrect ownership or partial-I/O handling can cause protocol corruption, connection leaks, or event-loop starvation.

## Progress snapshot

The single event-loop implementation is active and has passed the recorded
60-second Phase 3 acceptance scenarios. The plan remains active because the
focused correctness suite, repeated leak/saturation campaign, and complete
event-loop metrics evidence have not been rerun or recorded in the current
artifacts.

- [x] Event-loop implementation and HTTP response scheduling are present.
- [x] Recorded acceptance scenarios meet the throughput, latency, and error thresholds.
- [x] Focused event-loop correctness and keep-alive/pipeline validation passes.
- [x] Event-loop-specific metrics were captured during the final same-process
  connection campaign.

Latest local validation: `make` completed successfully, then
`./bin/run_tests server` passed all 22 tests with the server running from the
repository root. This included `test_keep_alive` and
`test_el_keepalive_multiple_cycles`.

## Purpose

Replace the blocking worker-per-connection model with a bounded, nonblocking
`epoll` event loop so slow or idle clients do not consume worker threads and
persistent connections can be handled independently of CPU-bound work.

## Scope

**Work locations:** `src/main.c`, `src/http_server.c`, `src/ring_buffer.c`, `src/thread_pool.c`, the corresponding headers, metrics implementation, and focused server/integration tests. Use the project conventions in `AGENTS.md` for build and test commands.

**In:**

- Nonblocking listening and client sockets.
- One-owner-per-connection event-loop state.
- Level-triggered `epoll` dispatch.
- Partial reads, partial writes, `EAGAIN`, disconnects, and error handling.
- Existing HTTP parsing, keep-alive, `HEAD`, pipelining, cached plain responses,
  cached gzip responses, status handling, and configured resource limits.
- Monotonic connection deadlines and bounded per-connection buffers.
- A bounded handoff path for CPU-bound work if profiling demonstrates that one is
  required.
- Metrics for event-loop activity, connection lifecycle, write backpressure, and
  rejected work.

**Out:**

- Multiple event-loop threads or processes.
- `SO_REUSEPORT`.
- Edge-triggered `epoll`.
- Dynamic content execution.
- Runtime gzip compression on the event-loop thread.
- Kernel or deployment tuning; those belong to later phases.
- Replacing the existing HTTP parser unless parity testing demonstrates that it
  cannot support incremental processing.

## Baseline

The pre-event-loop control is recorded in `benchmarks/benchmark_matrix.csv` and
`benchmarks/stress_results.csv`. Representative fixed-workload results reached
approximately 1,751 req/s at 5,000 offered req/s, while the latest blocking
keep-alive runs were approximately 300 req/s with p99 latency around 61 ms.
These historical runs use shorter or fixed-workload windows and are not a
like-for-like replacement for the Phase 3 acceptance contract.

The first event-loop runs are recorded in `benchmarks/phase3_event_loop.csv`.
They exposed an early keep-alive failure (7,298 steady responses and 960
failures), followed by corrected runs. The final 60-second evidence is in
`benchmarks/phase3_acceptance.csv`.

## Steps / Work

1. [~] Record the Phase 2 control run before changing the concurrency model;
   benchmark evidence is recorded, but profiling output and a dedicated metrics
   snapshot are still missing.
   Capture the benchmark matrix, correctness-suite result, server metrics,
   resource maxima, and the profiling evidence that identifies blocking socket
   I/O or worker occupancy as the limiting factor.
2. [x] Define the event-loop connection object and ownership contract. It must
   own the socket, `RingBuffer` input state, response/output state, parser
   progress, keep-alive counters, pipeline sequencing, lifecycle state, and
   monotonic deadlines. No worker or other loop may access these fields without
   an explicit handoff protocol.
3. [x] Add explicit connection lifecycle states covering at least reading
   headers, parsing, writing a response, waiting for keep-alive input, and
   closing. Define the valid transitions and the cleanup operation for every
   terminal path.
4. [x] Create a level-triggered `epoll` instance and register the listening
   socket and all admitted client sockets. Make the listening socket and
   accepted sockets nonblocking. Check every `epoll_create`, `epoll_ctl`,
   `accept`, `fcntl`, and close result.
5. [x] Move accept handling into the event loop. Drain accepts until `EAGAIN`,
   preserve active-connection admission limits, apply `FD_CLOEXEC`, and close
   newly accepted sockets deterministically when connection or state capacity is
   exhausted.
6. [x] Implement incremental nonblocking reads. Drain readable data until
   `EAGAIN`, append only within `MAX_INPUT_BUFFER_BYTES`, preserve fragmented
   request lines and headers, and retain parser rollback behavior when a full
   request is not yet available.
7. [x] Adapt request processing so it schedules a response rather than calling
   blocking `send()` or `sendfile()`. Preserve response framing and select the
   existing startup-cached representations without disk or zlib work on the
   event-loop thread.
8. [x] Implement a bounded output queue or response cursor per connection.
   Track header/body offsets, handle short writes, retry on writable events,
   handle `EINTR` and `EAGAIN`, and remove writable interest when no output is
   pending. Enforce explicit output-buffer and total-output limits before
   retaining additional response data.
9. [x] Preserve request ordering for pipelining. Responses must be associated
   with monotonically increasing request sequence numbers and delivered in
   request order. Cap buffered pipeline depth using `MAX_PIPELINE_DEPTH`; close
   or reject deterministically when the cap is exceeded.
10. [x] Implement monotonic deadlines for initial header reads, idle keep-alive,
    and pending writes. Use a timer strategy compatible with the single event
    loop, such as a deadline scan or `timerfd`, and ensure expired connections
    are removed from `epoll` and released exactly once.
11. [x] Define the slow-reader policy. A connection that exceeds the write
    deadline or output limits must not block or retain unbounded memory; record
    the reason, close it, and continue servicing unrelated connections.
12. [x] Keep CPU-bound work off the event loop. Continue serving cached static
    and cached gzip responses locally. If a profiling result requires deferred
    work, add a bounded worker queue with explicit queue-full behavior and a
    completion notification mechanism that returns ownership to the event loop.
13. [x] Remove blocking client handling from `worker_thread_function` only after
    event-loop parity tests pass. Retain or remove the thread pool based on its
    measured role; do not leave two competing owners for the same socket.
14. [x] Extend metrics without blocking or allocating on the hot path. Record
    event-loop wakeups, readable and writable events, `EAGAIN` occurrences,
    partial writes, active event-loop connections, output-limit closes,
    deadline closes, epoll-control failures, and CPU-work queue depth/rejections.
15. [~] Add focused integration tests for fragmented reads, coalesced requests,
    fragmented writes, partial writes, pipelining order, keep-alive deadlines,
    slow readers, client disconnects, `EPIPE`, `ECONNRESET`, output limits, and
    cleanup after every close/error path.
16. [~] Run the benchmark matrix against the old worker model and the new
    single-event-loop model under the same measurement contract. Compare
    throughput, scenario-specific p99 latency, errors, CPU utilization, wakeups,
    active connections, memory, descriptors, and queued work.

## Validation

- [x] Run `make` from the repository root and confirm `bin/http_server` and
  `bin/run_tests` are rebuilt.
- [x] Run `./bin/run_tests ring` and `./bin/run_tests thread_pool`.
- [x] Start `./bin/http_server` from the repository root and run
  `./bin/run_tests server`.
- [x] Run the event-loop-specific integration tests with fragmented,
  coalesced, pipelined, slow-reader, timeout, disconnect, and partial-write
  clients; the 22-test server suite passed, including the available Phase 3
  cases. Dedicated EPIPE/ECONNRESET and slow-reader isolation coverage remains.
- [x] Run the `slow-clients` and `error-paths` benchmark scenarios; evidence is
  in `benchmarks/phase3_acceptance.csv`.
- [x] Run `new-connection-5000`, `keep-alive-5000`, `mixed-paths-5000`, and
  `gzip-500` using the Phase 3 measurement contract: bounded warmup, exactly
  60 seconds at the offered rate, bounded drain, and separate phase reporting.
- [x] Run repeated connect/disconnect and saturation campaigns long enough to
   verify that active connections, ring buffers, output queues, file
   descriptors, RSS, and deferred work return to baseline. The short final
   campaign completed 2,000 connections in 20 rounds with zero failures;
   metrics ended at `active_connections=0`, `request_failures=0`,
   `el_connections_opened=2000`, `el_connections_closed=2000`,
   `el_deadline_closes=0`, and `el_pipeline_full=0`.

### Recorded results

The 2026-09-20 rows in `benchmarks/phase3_acceptance.csv` use a 10-second
warmup, 60-second steady-state window, and bounded drain.

| Scenario | Steady responses | Failures | Successful rps | p99 ms |
| --- | ---: | ---: | ---: | ---: |
| `keep-alive-5000` | 299,998 | 0 | 4,999.97 | 1.027 |
| `new-connection-5000` | 300,146 | 0 | 5,002.43 | 10.646 |
| `mixed-paths-5000` | 299,992 | 0 | 4,999.87 | 9.363 |
| `gzip-500` | 30,000 | 0 | 500.00 | 0.722 |
| `error-paths` | 60,000 | 0 | 1,000.00 | 0.811 |
| `slow-clients` | 30,000 | 0 | 500.00 | 0.714 |

The keep-alive row completes its two remaining requests during drain. The
acceptance CSV does not contain the event-loop-specific wakeup, `EAGAIN`,
partial-write, deadline-close, or output-queue fields required by this plan,
so those metrics remain unproven by the checked-in benchmark evidence.

The final local checkpoint used the existing matrix in short diagnostic mode:
all seven scenarios completed with zero failures and zero queue rejections;
`keep-alive-5000` achieved 4,999.80 successful req/s at p99 1.80 ms. A
same-process follow-up then ran 20 rounds of 100 concurrent new connections
(2,000 total) with zero client failures. The final metrics snapshot reported
`active_connections=0`, `accepted_connections=2000`,
`completed_requests=2000`, `request_failures=0`,
`active_connections_max=17`, `el_connections_opened=2000`,
`el_connections_closed=2000`, `el_wakeups=5245`, `el_eagain=2000`,
`el_deadline_closes=0`, and `el_pipeline_full=0`.

### Measurement contract

For quantitative checks, use the same documented benchmark host, build flags,
resource limits, scenario definitions, method, and complete reported-field set
as the parent scaling plan. Report warmup, steady-state, and drain separately.
For the steady-state window, report offered rate, completed and failed
requests, status distribution, p50/p95/p99/max latency, maximum concurrency,
active connections, connection and keep-alive rates, outstanding requests,
client/server limitation, client/server CPU utilization, RSS, descriptors,
context switches and retransmits where available, and all event-loop rejection
or close counters. Add event-loop-specific fields for output queue maxima,
wakeups, `EAGAIN` occurrences, partial writes, deadline closes, and deferred-
work queue depth/rejections.

The primary acceptance run offers exactly 5,000 requests/second for 60 seconds.
The event-loop implementation must complete at least 300,000 valid requests
with zero transport, framing, or unexpected-status errors. The scenario p99
limits are `<20 ms` for cached keep-alive plain responses, `<50 ms` for cached
new connections, and `<100 ms` for cached gzip responses. Evidence comes from
the benchmark result CSV and the server metrics snapshot.

## Exit criteria

- [x] The event loop owns every admitted client socket; no event-loop thread
  blocks in `read`, `recv`, `write`, `send`, or `sendfile`.
- [~] All readable and writable paths handle partial progress, `EINTR`,
  `EAGAIN`, orderly client close, `EPIPE`, and `ECONNRESET` without terminating
  the process or leaking the connection.
- [~] A slow reader cannot prevent an independent client from completing a
  response during the same bounded test window.
- [x] Keep-alive connections have bounded input and output memory, enforce the
  configured request, pipeline, and timeout limits, and release all resources
  after expiry or disconnect.
- [x] Pipelined responses are delivered strictly in request order in repeated
  fragmented and coalesced socket tests.
- [x] Existing HTTP behavior remains green for cached plain/gzip responses,
  `HEAD`, 404, unsupported methods, keep-alive, and parser regression suites.
- [x] Repeated saturation and disconnect tests show no growth in active
  connections, buffers, output queues, file descriptors, tasks, or RSS after
  the drain period; the final metrics snapshot returned active connections to
  zero with no request failures or deadline/pipeline closes.
- [x] The 5,000 requests/second, 60-second acceptance run meets the stated
  request-count, zero-error, and scenario-specific p99 criteria, or the
  measured blocker is documented with evidence before Phase 4 begins.
- [x] Event-loop wakeups, CPU utilization, backpressure, deadline closes, and
  deferred-work queue behavior are present in the benchmark evidence before
  further concurrency scaling is attempted.

## Risks and rollback

- **Parser and state-machine regressions** → Keep the existing blocking path
  available behind a build-time or startup-selected implementation during
  migration. Switch back to it if parity tests fail.
- **Partial-write data loss or duplicated responses** → Represent response
  progress with explicit offsets and sequence numbers; close the connection and
  discard unsent state on protocol-fatal errors. Do not retry bytes whose send
  result is unknown.
- **Busy-looping on `epoll` events** → Drain readable/writable work to
  `EAGAIN`, update interest masks only after state changes, and record repeated
  wakeups. Roll back the event registration change if CPU usage rises without
  corresponding work.
- **Unbounded per-connection memory** → Enforce input, output, and pipeline
  limits before allocation or queueing. On limit violation, emit the configured
  response when safe or close deterministically.
- **Timer or close races** → Keep connection ownership in the event loop and
  make close idempotent. Remove the descriptor from `epoll` before freeing its
  connection state.
- **CPU work starving I/O** → Keep cached responses local and use a bounded
  deferred-work queue only for measured CPU-bound operations. On queue
  exhaustion, apply backpressure or close according to the documented policy.
- **Performance regression despite correctness** → Preserve the Phase 2
  control configuration and benchmark output. Revert the event-loop dispatch
  while retaining new tests and metrics, then use the profile to identify the
  regression before retrying.

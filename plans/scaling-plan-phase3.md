---
plan_id: scaling-phase-3-event-loop
title: Phase 3 Event-Loop Socket I/O
category: implementation
status: active
owner: agent
created: 2026-09-19
updated: 2026-09-19
related: []
---

# Phase 3 Event-Loop Socket I/O

- **Phase ID:** `scaling-phase-3-event-loop`
- **Objective:** Make the single event-loop implementation own all admitted client sockets while preserving HTTP behavior and bounded resource use.
- **Complexity:** 4 — This is a cross-module concurrency-model change with interacting ownership, parser, buffering, deadline, and ordering invariants; it remains one bounded phase because the event-loop transition must be validated as an integrated protocol path.
- **Risk:** high — Incorrect ownership or partial-I/O handling can cause protocol corruption, connection leaks, or event-loop starvation.

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

The Phase 3 control baseline is not yet recorded. Work item 1 must capture it before the concurrency-model change, using the parent scaling plan's measurement contract and the existing Phase 2 implementation. Evidence must include the benchmark matrix, correctness result, metrics snapshot, resource maxima, and profiling output; until that run exists, no Phase 3 performance comparison is asserted.

## Steps / Work

1. [ ] Record the Phase 2 control run before changing the concurrency model.
   Capture the benchmark matrix, correctness-suite result, server metrics,
   resource maxima, and the profiling evidence that identifies blocking socket
   I/O or worker occupancy as the limiting factor.
2. [ ] Define the event-loop connection object and ownership contract. It must
   own the socket, `RingBuffer` input state, response/output state, parser
   progress, keep-alive counters, pipeline sequencing, lifecycle state, and
   monotonic deadlines. No worker or other loop may access these fields without
   an explicit handoff protocol.
3. [ ] Add explicit connection lifecycle states covering at least reading
   headers, parsing, writing a response, waiting for keep-alive input, and
   closing. Define the valid transitions and the cleanup operation for every
   terminal path.
4. [ ] Create a level-triggered `epoll` instance and register the listening
   socket and all admitted client sockets. Make the listening socket and
   accepted sockets nonblocking. Check every `epoll_create`, `epoll_ctl`,
   `accept`, `fcntl`, and close result.
5. [ ] Move accept handling into the event loop. Drain accepts until `EAGAIN`,
   preserve active-connection admission limits, apply `FD_CLOEXEC`, and close
   newly accepted sockets deterministically when connection or state capacity is
   exhausted.
6. [ ] Implement incremental nonblocking reads. Drain readable data until
   `EAGAIN`, append only within `MAX_INPUT_BUFFER_BYTES`, preserve fragmented
   request lines and headers, and retain parser rollback behavior when a full
   request is not yet available.
7. [ ] Adapt request processing so it schedules a response rather than calling
   blocking `send()` or `sendfile()`. Preserve response framing and select the
   existing startup-cached representations without disk or zlib work on the
   event-loop thread.
8. [ ] Implement a bounded output queue or response cursor per connection.
   Track header/body offsets, handle short writes, retry on writable events,
   handle `EINTR` and `EAGAIN`, and remove writable interest when no output is
   pending. Enforce explicit output-buffer and total-output limits before
   retaining additional response data.
9. [ ] Preserve request ordering for pipelining. Responses must be associated
   with monotonically increasing request sequence numbers and delivered in
   request order. Cap buffered pipeline depth using `MAX_PIPELINE_DEPTH`; close
   or reject deterministically when the cap is exceeded.
10. [ ] Implement monotonic deadlines for initial header reads, idle keep-alive,
    and pending writes. Use a timer strategy compatible with the single event
    loop, such as a deadline scan or `timerfd`, and ensure expired connections
    are removed from `epoll` and released exactly once.
11. [ ] Define the slow-reader policy. A connection that exceeds the write
    deadline or output limits must not block or retain unbounded memory; record
    the reason, close it, and continue servicing unrelated connections.
12. [ ] Keep CPU-bound work off the event loop. Continue serving cached static
    and cached gzip responses locally. If a profiling result requires deferred
    work, add a bounded worker queue with explicit queue-full behavior and a
    completion notification mechanism that returns ownership to the event loop.
13. [ ] Remove blocking client handling from `worker_thread_function` only after
    event-loop parity tests pass. Retain or remove the thread pool based on its
    measured role; do not leave two competing owners for the same socket.
14. [ ] Extend metrics without blocking or allocating on the hot path. Record
    event-loop wakeups, readable and writable events, `EAGAIN` occurrences,
    partial writes, active event-loop connections, output-limit closes,
    deadline closes, epoll-control failures, and CPU-work queue depth/rejections.
15. [ ] Add focused integration tests for fragmented reads, coalesced requests,
    fragmented writes, partial writes, pipelining order, keep-alive deadlines,
    slow readers, client disconnects, `EPIPE`, `ECONNRESET`, output limits, and
    cleanup after every close/error path.
16. [ ] Run the benchmark matrix against the old worker model and the new
    single-event-loop model under the same measurement contract. Compare
    throughput, scenario-specific p99 latency, errors, CPU utilization, wakeups,
    active connections, memory, descriptors, and queued work.

## Validation

- [ ] Run `make` from the repository root and confirm `bin/http_server` and
  `bin/run_tests` are rebuilt.
- [ ] Run `./bin/run_tests ring` and `./bin/run_tests thread_pool`.
- [ ] Start `./bin/http_server` from the repository root and run
  `./bin/run_tests server`.
- [ ] Run the event-loop-specific integration tests with fragmented,
  coalesced, pipelined, slow-reader, timeout, disconnect, and partial-write
  clients.
- [ ] Run the `slow-clients` and `error-paths` benchmark scenarios.
- [ ] Run `new-connection-5000`, `keep-alive-5000`, `mixed-paths-5000`, and
  `gzip-500` using the Phase 3 measurement contract: bounded warmup, exactly
  60 seconds at the offered rate, bounded drain, and separate phase reporting.
- [ ] Run repeated connect/disconnect and saturation campaigns long enough to
  verify that active connections, ring buffers, output queues, file
  descriptors, RSS, and deferred work return to baseline.

### Measurement contract

For quantitative checks, use the same documented benchmark host, build flags,
resource limits, and scenario definitions as `scaling-plan`. Report warmup,
steady-state, and drain separately. For the steady-state window report offered
For quantitative checks, use the same documented benchmark host, build flags, resource limits, scenario definitions, method, and complete reported-field set as the parent scaling plan. Report warmup, steady-state, and drain separately. For the steady-state window, report offered rate, completed and failed requests, status distribution, p50/p95/p99/max latency, maximum concurrency, active connections, connection and keep-alive rates, outstanding requests, client/server limitation, client/server CPU utilization, RSS, descriptors, context switches and retransmits where available, and all event-loop rejection or close counters. Add event-loop-specific fields for output queue maxima, wakeups, `EAGAIN` occurrences, partial writes, deadline closes, and deferred-work queue depth/rejections.

The primary acceptance run offers exactly 5,000 requests/second for 60 seconds.
The event-loop implementation must complete at least 300,000 valid requests
with zero transport, framing, or unexpected-status errors. The scenario p99
limits are `<20 ms` for cached keep-alive plain responses, `<50 ms` for cached
new connections, and `<100 ms` for cached gzip responses. Evidence comes from
the benchmark result CSV and the server metrics snapshot.

## Exit criteria

- [ ] The event loop owns every admitted client socket; no event-loop thread
  blocks in `read`, `recv`, `write`, `send`, or `sendfile`.
- [ ] All readable and writable paths handle partial progress, `EINTR`,
  `EAGAIN`, orderly client close, `EPIPE`, and `ECONNRESET` without terminating
  the process or leaking the connection.
- [ ] A slow reader cannot prevent an independent client from completing a
  response during the same bounded test window.
- [ ] Keep-alive connections have bounded input and output memory, enforce the
  configured request, pipeline, and timeout limits, and release all resources
  after expiry or disconnect.
- [ ] Pipelined responses are delivered strictly in request order in repeated
  fragmented and coalesced socket tests.
- [ ] Existing HTTP behavior remains green for cached plain/gzip responses,
  `HEAD`, 404, unsupported methods, keep-alive, and parser regression suites.
- [ ] Repeated saturation and disconnect tests show no growth in active
  connections, buffers, output queues, file descriptors, tasks, or RSS after
  the drain period.
- [ ] The 5,000 requests/second, 60-second acceptance run meets the stated
  request-count, zero-error, and scenario-specific p99 criteria, or the
  measured blocker is documented with evidence before Phase 4 begins.
- [ ] Event-loop wakeups, CPU utilization, backpressure, deadline closes, and
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

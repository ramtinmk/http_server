# HTTP Server Scaling Plan

## Purpose

This document defines the path from the current educational HTTP server to a
server that can sustain 5,000 requests per second for the supported static
endpoints. The plan prioritizes measurable improvements, protocol correctness,
and predictable failure behavior over premature micro-optimizations.

The target is **sustained successful throughput**, not merely accepting 5,000
connections per second, and not merely draining a fixed workload. A valid
milestone must be measured over a fixed-rate, fixed-duration window with zero
unexpected request errors and an explicit latency objective.

## Current Baseline

The current implementation has these characteristics:

- A single blocking `accept()` loop.
- A fixed pool of 16 worker threads.
- One worker handles a client socket, including keep-alive idle time.
- Blocking socket reads and writes.
- `BACKLOG` is configured as 10.
- Access logging on the accept path is synchronous (`printf()`), gated by
  `HTTP_SERVER_ACCESS_LOG`; throughput runs set it to `0`.
- Static files are opened and stat'ed for every request.
- `sendfile()` is used for uncompressed responses.
- Gzip responses are compressed on every request.
- The benchmark defaults to a new TCP connection for every request.
- Server counters (accepted, completed, response classes, queue depth, active
  workers, rejected tasks) are exposed through `HTTP_SERVER_METRICS_FILE`.

The preserved 5,000 req/s stress target offered 50,000 requests. The server
completed all requests without HTTP errors after increasing the client timeout,
but the run took 26.416 seconds and achieved approximately 1,893 req/s. This
demonstrates that the server can eventually drain the workload, but does not
yet sustain the offered rate.

The first benchmark result should not be treated as a final capacity number.
It combines request processing, TCP connection setup, accept backlog behavior,
client scheduling, and queue drain time. Every future result must report both
offered load and completed throughput.

The benchmark was subsequently rebuilt to record the full Measurement Contract
and to run the scenario matrix. The first logged run with that harness offered
the same 50,000 requests and completed all of them with zero errors in 28.6
seconds (approximately 1,751 req/s); see `benchmarks/stress_results.csv` and
`benchmarks/benchmark_matrix.csv`. That run is still a **fixed-workload drain
test**, not yet a sustained-rate test; converting it is part of Phase 0.

## Progress Snapshot

Legend: `[x]` done, `[~]` partial, `[ ]` pending.

- [x] Phase 0: counters, structured output, sustained-rate phases, per-second
  reporting, and environment fingerprinting are implemented. Repeatability is
  an operational acceptance check performed on the target benchmark host.
- [~] Phase 1: logging, startup static/gzip caching, and configurable `BACKLOG`
  are implemented; buffer reuse, profiling, and performance exit criteria remain.
- [ ] Profiling checkpoint (after Phase 1).
- [~] Phase 2: connection and queue capacity, explicit resource limits are implemented; saturation validation and several fine-grained limits remain.
- [~] Phase 3: the single event-loop implementation, keep-alive correctness,
  and recorded acceptance scenarios are validated; long saturation/cleanup
  coverage and complete event-loop metrics evidence remain. See
  `scaling-phase-3-event-loop`.
- [ ] Phase 4: scale accept and CPU work.
- [ ] Phase 5: operating-system and deployment tuning.

## Goals and Non-Goals

### Goals

- Meet this precise acceptance criterion: **during a 60-second steady-state
  window, complete at least 300,000 valid requests offered at exactly 5,000
  requests/second, with zero transport, framing, or unexpected-status errors,
  p99 latency below the scenario-specific limit, and no unbounded growth in
  active connections, memory, file descriptors, or queued work.**
- Define latency per scenario rather than with a single number:

  | Scenario | Steady-state p99 target |
  | --- | --- |
  | Cached keep-alive (plain) | < 20 ms |
  | Cached new connection (plain) | < 50 ms |
  | Cached gzip | < 100 ms |

  Latency targets apply only to the steady-state window (warmup and drain are
  excluded), count only successful responses (failures are reported separately),
  are per request, and for new-connection scenarios include TCP connection
  setup in the measured request.
- Separate target tiers so gzip does not gate the primary milestone:
  - Primary target: cached, non-gzip static responses.
  - Secondary target: cached gzip responses.
  - Future target: uncached or dynamic compression.
- Preserve HTTP/1.1 keep-alive, pipelining, `HEAD`, 404, 501, and gzip behavior.
- Keep overload behavior bounded: memory, file descriptors, and queues must not
  grow without limit.
- Make benchmark runs reproducible in CI or on a documented test host.

### Non-goals for the first milestone

- TLS termination.
- Dynamic application execution.
- General-purpose static file serving from arbitrary paths.
- Distributed load balancing.
- Optimizing gzip for large, highly compressible assets before the plain static
  path is stable.

## Measurement Contract

Before and after each phase, run the same benchmark matrix. The canonical
acceptance run is a **fixed-rate, fixed-duration** test, not a fixed-workload
drain:

1. Warm up for a bounded period (for example 10 seconds) so pools, caches, and
   the TCP path reach steady state; discard warmup results.
2. Offer exactly the target rate for the measurement window (for example 60
   seconds at 5,000 req/s) using fixed-rate scheduling.
3. Stop sending new requests at the end of the window.
4. Allow a bounded drain period for in-flight requests; report drain separately.
5. Report warmup, steady-state, and drain as distinct phases.

This prevents a burst-and-catch-up server from passing by draining during the
drain period. Record:

- Warmup duration, measurement duration, and drain duration.
- Offered request rate (aggregate and per second).
- Completed and failed requests, split per phase.
- Achieved requests per second (aggregate and per second).
- Requests completed during drain.
- Status-code distribution.
- p50, p95, p99, and maximum latency for the steady-state window.
- Maximum concurrency and number of outstanding requests.
- Connection rate and keep-alive request rate.
- Whether the **client or the server** limited throughput.
- Client CPU utilization and client-side socket errors.
- Server CPU utilization, RSS, and accepted connections.
- Server accept failures.
- Open file descriptors on both sides.
- Thread-pool queue depth and rejected tasks.
- System context switches and network retransmits where available.
- Hardware fingerprint metadata: machine ID, CPU model and topology, memory,
  kernel, compiler flags, page size, and file-descriptor limits.
- Server CPU cost per 1,000 completed requests and
  `hardware_agnostic_rps` (successful requests per server CPU-second). This is
  the primary cross-hardware stress-test gate; raw `throughput_rps` remains a
  diagnostic value.
- Calibration-backed `successful_rps_normalized` and normalized p50/p95/p99
  latency. The calibration index and whether calibration was enabled are
  recorded in every result row. See `plans/hardware-agnostic-benchmark.md` for
  the implementation and comparison rules.

Use separate scenarios because they exercise different bottlenecks. The
identifiers in parentheses match `scripts/http_benchmark.py --scenario`:

| Scenario | CLI name | Purpose |
| --- | --- | --- |
| 1,000 req/s, new connection | `new-connection-1000` | Establish connection and accept baseline |
| 5,000 req/s, new connection | `new-connection-5000` | Stress accept, backlog, and connection handling |
| 5,000 req/s, keep-alive | `keep-alive-5000` | Measure request processing without TCP setup |
| 5,000 req/s, `/home` and `/hello` mix | `mixed-paths-5000` | Avoid endpoint-specific conclusions |
| 500 req/s gzip | `gzip-500` | Measure cached gzip response handling |
| Slow clients and idle keep-alive | `slow-clients` | Verify resource protection |
| 404 and unsupported methods | `error-paths` | Measure error-path behavior |

The benchmark must distinguish a server that is slow from a client that has
finished sending load. Use response framing (`Content-Length`, chunked encoding,
or close-delimited bodies) and never an idle read timeout to determine request
completion.

## Phase 0: Establish a Reliable Baseline

### Work

- [x] Keep the dependency-free benchmark as the canonical load generator.
- [x] Add a benchmark result format suitable for automation, such as JSON or
  CSV, while retaining human-readable output.
- [x] Add server counters for accepted connections, completed requests,
  response classes, active workers, queue depth, and request failures.
- [x] Make logging configurable and explicitly disable access logging during
  throughput measurements (`HTTP_SERVER_ACCESS_LOG`).
- [x] Convert the benchmark from a fixed-workload drain test to a true
  fixed-rate, fixed-duration sustained test with warmup, steady-state, and drain
  phases.
- [x] Record per-second offered and completed rates so bursts and stalls are
  visible, plus the number of requests completed during drain.
- [x] Report whether the client or the server limited throughput, including
  client CPU utilization and client-side socket errors.
- [x] Pin down the benchmark host, CPU count, kernel, compiler flags, and ulimit
  values in the test documentation.

### Exit criteria

- [ ] A 60-second steady-state run can be repeated with less than 5% throughput
  variation.
- [ ] Every request is accounted for as successful or failed, per phase.
- [x] A failed run identifies whether the failure was connect, send, receive,
  framing, status, or timeout related.
- [x] A sustained run reports warmup, steady state, and drain separately, with
  per-second rates.

## Phase 1: Remove Avoidable Per-Request Work

This phase should be completed before changing the concurrency model.

### Work

- [x] Guard the accept-path `printf()` calls behind a configurable log level
  that throughput runs disable.
- [x] Raise `BACKLOG` from 10 to a configurable value such as 1024. Treat this
  as burst tolerance: it is unlikely to be the main sustained-throughput
  lever.
- [ ] Verify the effective backlog limit against the kernel `somaxconn` value.
- [x] Load `home.html` and `hello.html` once at startup.
- [x] Precompute the plain and gzip response headers and body lengths.
- [x] Keep cached response bytes in memory and serve them directly. Prefer a
  complete response representation (headers + body + encoding) selected in the
  hot path over re-formatting headers per request, for example:

  ```c
  struct response_variant {
      const char *body;
      size_t body_len;
      const char *content_encoding;
      const char *content_type;
      char headers[256];
      size_t headers_len;
  };
  ```

- [x] Cache gzip output for each static asset instead of running zlib per
  request.
- [ ] Reuse per-worker request buffers where safe.
- [~] Avoid repeated path formatting and unnecessary string work in the hot
  path; path formatting is removed, but broader string-work cleanup remains.

### Risks

- Cached files can become stale if development-time file replacement is
  expected. Use an explicit reload or development mode rather than silently
  changing production semantics.
- Cached variants must get negotiation right: `HEAD` sends the same headers with
  the correct `Content-Length` but no body; honor `Accept-Encoding` including
  `gzip;q=0`, `identity`, missing, and malformed values; emit
  `Vary: Accept-Encoding`; and keep keep-alive connection headers correct.

### Exit criteria

- [ ] Plain cached keep-alive traffic reaches the 5,000 req/s acceptance
  criterion with zero unexpected errors on the benchmark host, or profiling
  proves the remaining limit is the blocking connection architecture.
- [ ] Steady-state p99 meets the scenario-specific target (20 ms keep-alive,
  50 ms new connection, 100 ms gzip).
- [x] Gzip correctness tests still pass byte-for-byte after decompression.

Implementation validation passes the build, ring-buffer, thread-pool, and
server suites, including cached plain/gzip responses, `HEAD`, `gzip;q=0`,
404, keep-alive, and pipelining. Short plain and gzip benchmark smoke runs also
completed with zero failures; the full 5,000 req/s acceptance run and profiling
checkpoint remain pending.

Baseline runs to date show roughly 1.5k-2.1k req/s for new connections and
keep-alive starvation at 5,000 req/s, which is consistent with the blocking
worker-per-connection model, but no profiler trace has been captured yet.

## Profiling Checkpoint (after Phase 1)

Capture a profile after caching and before committing to the event-loop rewrite.
The event loop is probably the right eventual architecture, but the profile
should show whether the immediate limit is:

- 16 blocked workers.
- Connection setup and accept rate.
- Client-side scheduling or client CPU.
- Synchronous logging or another hidden serialization point.
- Kernel backlog pressure.
- Gzip/CPU work.
- Mutex contention or allocator activity.
- A lock, response formatting, or a benchmark bug.

Instrument or profile at minimum: `accept()` rate, time in `read()`, `write()`,
parsing, and gzip; mutex contention; allocator activity; context switches and
event-loop wakeups (once they exist); cache misses where available. Gating
`printf()` is necessary but not sufficient: logging may not be the only hidden
serialization point.

## Phase 2: Fix Connection and Queue Capacity

The current pool has only 16 workers, and each worker can remain blocked on an
idle client. Increasing the thread count alone is not a durable solution, but
the queue and descriptor limits must still be explicit.

The queue model depends on the architecture, and the plan should not conflate
the two:

- In the current blocking design (`accept -> task queue -> worker owns socket`),
  a bounded task queue protects task memory only. It does **not** free workers
  from idle connections.
- In the event-loop design (event loop owns socket, loop -> bounded work queue
  for expensive CPU tasks only), the queue bounds CPU work rather than
  connections.

Fixing Phase 2 by increasing worker count and queue size can make slow-client
behavior worse, so the limits below belong to the resource-protection design and
not only to the regression section.

### Work

- [x] Define a bounded maximum number of queued tasks with `MAX_QUEUED_TASKS`
  and track `queue_length` under the queue mutex.
- [x] Close newly accepted connections deterministically when the queue or task
  arena is full; enqueue returns a status and never allocates unbounded task
  memory.
- [~] Define and enforce explicit resource limits. Implemented limits include
  `MAX_ACTIVE_CONNECTIONS`, `MAX_INPUT_BUFFER_BYTES`,
  `MAX_KEEPALIVE_REQUESTS`, `MAX_PIPELINE_DEPTH`, header-read timeout, idle
  keep-alive timeout, and write timeout. Request-line/header-size semantics,
  output-byte limits, and a separate output-buffer limit remain to be added.
- [x] Make the task arena and buffer pool bounded. Buffer acquisition returns
  `NULL` on pool exhaustion instead of allocating an uncapped fallback; sizing
  still needs benchmark-based tuning.
- [~] Check `accept`, task enqueue, buffer acquisition, and send results. The
  main admission and send paths are checked; accept errno classification,
  `fcntl` failure handling, and complete per-errno metrics remain.
- [x] Set `FD_CLOEXEC` on the listening and accepted sockets and report the
  required `RLIMIT_NOFILE` headroom at startup. Kernel-limit enforcement and
  deployment documentation remain.
- [x] Add queue-depth, active-worker, active-connection, and overload metrics.
- [ ] Test worker counts around the available CPU count instead of assuming
  that more threads always improve throughput.
- [~] Verify `listen()`'s return value and handle accept errors. Effective
  `somaxconn`, SYN backlog/completed-connection queues, listen drops, and
  backlog observability remain. `SO_REUSEADDR` is already set; keep
  `SO_REUSEPORT` for Phase 4.

### Phase 2 implementation status

The current implementation is in `src/main.c`, `src/http_server.c`,
`src/thread_pool.c`, `src/metrics.c`, and `include/server_config.h`.
Configuration values are compile-time overridable and are printed at startup.
The default policy is 16 workers, 256 queued tasks, 512 active connections,
64 KiB input buffering, a 16-request pipeline depth, 100 requests per
keep-alive connection, a 5-second initial header timeout, a 30-second idle
keep-alive timeout, and a 10-second write timeout.

The current overload policy is close-on-admission-failure: active-connection,
queue, task-arena, and buffer-pool exhaustion are counted and the client socket
is closed rather than waiting in the accept loop. The implementation remains
the blocking worker-per-connection architecture; Phase 2 does not remove the
worker's responsibility for a connected socket.

Focused validation now covers queue configuration, task admission, strict
buffer-pool exhaustion, slow clients that send no headers, keep-alive request
limits, and oversized input. The full sustained saturation/leak campaign and
kernel backlog measurements are still pending.

### Exit criteria

- [~] Saturation tests do not leak file descriptors, buffers, or tasks. Focused
  cleanup tests pass, but repeated connect/disconnect, descriptor-exhaustion,
  RSS, and long-running saturation measurements remain.
- [x] The process remains responsive when clients connect and do not send
  headers; the header-read timeout is covered by an integration test.
- [x] Queue-full/task-pool behavior is bounded and visible in metrics. A
  dedicated queue-full integration test and per-reason rejection metrics are
  still useful follow-ups.
- [~] The configured limits are observable and enforced. Startup output and
  metrics expose the implemented limits/counters; fine-grained header/output
  limits and effective kernel backlog values remain.

## Phase 3: Move Socket I/O to an Event Loop

This is the primary architectural change if Phase 1 and profiling show that
blocking workers are the limit. The current blocking worker-per-connection
design cannot scale efficiently when many persistent or slow connections exist.

### Target architecture

- One or more event-loop threads own nonblocking listening and client sockets.
- `epoll` watches accept, readable, and writable events.
- Each connection has explicit read, parse, write, and close state.
- Event loops perform lightweight HTTP parsing and response scheduling.
- A bounded worker pool handles only CPU-bound operations such as uncached
  compression or future dynamic content.
- A connection is never allowed to block an event-loop thread on disk, socket
  output, or a slow peer.

### Model decisions to make explicit

- [ ] Use level-triggered `epoll` initially. Edge-triggered mode can be faster
  in some designs but requires draining every readable/writable condition
  correctly and handling `EAGAIN` perfectly; treat it as a later optimization
  after profiling.
- [ ] Enforce one owner per connection. The owning event loop owns the socket,
  parser state, input buffer, output queue, and keep-alive timer. Other threads
  must not touch that connection directly unless a safe handoff mechanism is
  defined.
- [ ] Do not block on `sendfile()`. On a nonblocking socket it can report
  positive progress, `-1` with `EAGAIN`, partial progress, or another error. The
  connection state must retain the current file offset and register writable
  interest when needed. For cached in-memory responses, `writev()`/`send()` may
  be simpler and fast enough for the first target.
- [ ] Add explicit connection lifecycle states, for example `READING_HEADERS`,
  `PARSING`, `WRITING_RESPONSE`, `WRITING_PIPELINED_RESPONSE`, `KEEPALIVE_IDLE`,
  `CLOSING`. These make fragmented input, pipelining, timeouts, and partial
  output far easier to test.
- [ ] Preserve pipelined response ordering: response N must not overtake
  response N-1. For the first implementation, either process pipelined static
  requests synchronously in connection order or keep a response sequence number
  and an ordered response queue. Cap how many pipelined requests may be buffered
  per client.
- [ ] Keep gzip off the event loop. Precompress fixed assets at startup. If
  compression is ever dynamic, use a bounded compression queue, cap concurrent
  compressors, apply backpressure, define queue-full behavior, and never let
  slow clients retain compression buffers indefinitely.

### Migration sequence

1. [x] Introduce a connection-state structure without changing response
   semantics.
2. [x] Convert accepted client sockets to nonblocking mode.
3. [x] Implement header reads and response writes with partial-I/O handling.
4. [x] Add per-connection output limits and write-interest registration.
5. [x] Move keep-alive timeout handling to a monotonic timer mechanism.
6. [x] Preserve the existing parser and ring-buffer tests while adding
   fragmented, coalesced, and pipelined socket tests.
7. [x] Remove blocking client handling from the worker pool after parity tests
   pass.

### Exit criteria

- [ ] Slow-reader tests cannot stall unrelated clients.
- [x] Keep-alive traffic uses a bounded amount of memory per connection.
- [x] Pipelined responses are always delivered in request order.
- [x] 5,000 req/s new-connection and keep-alive runs pass the sustained-rate
  acceptance criterion with zero unexpected errors.
- [x] Event-loop CPU utilization and wakeups are measured before further
  tuning.

## Phase 4: Scale Accept and CPU Work

After nonblocking I/O is stable, scale only where profiling identifies a limit.

### Options

- [ ] Run multiple event-loop threads with clear ownership of connections.
- [ ] Use `SO_REUSEPORT` and multiple processes when separate accept queues
  improve distribution on the target kernel.
- [ ] Keep gzip work off the event loop and cap compression concurrency.
- [ ] Precompress known static assets during startup.
- [ ] Use `sendfile` or equivalent zero-copy output only after response caching
  and nonblocking writes are correct.

Avoid adding processes or threads solely to compensate for lock contention that
has not been measured.

## Phase 5: Operating-System and Deployment Tuning

Only apply these settings after application bottlenecks are addressed:

- [ ] Raise `RLIMIT_NOFILE` for the server and benchmark client.
- [ ] Verify `net.core.somaxconn` and TCP receive/send buffer limits.
- [ ] Use a dedicated benchmark host or isolate the server from competing CPU
  and I/O workloads.
- [ ] Record CPU frequency behavior and power-management settings.
- [ ] Consider CPU affinity only if scheduler migration is measurable.
- [~] Monitor SYN backlog overflow, retransmits, accept errors, and listen
  drops (retransmits and context switches are already recorded in CSV).

Every tuning change must be recorded with the kernel version and reverted in a
control run to prove it improves the application rather than the environment.

## Correctness and Regression Gates

No performance optimization should merge unless these remain green.

### HTTP parsing

- [x] Ring-buffer unit tests.
- [x] Fragmented request and fragmented response tests (`\r\n` split across
  reads).
- [x] Keep-alive and pipelining tests.
- [x] HEAD response tests.
- [x] Gzip decompression and chunked-framing tests.
- [x] Existing server endpoint tests.
- [~] 404 and unsupported-method tests (covered); 400, 413, and 431 are not yet
  exercised.
- [ ] Multiple spaces between method, path, and version.
- [ ] Header names with different casing.
- [ ] Duplicate headers.
- [ ] Conflicting `Content-Length`.
- [ ] `Transfer-Encoding` handling.
- [ ] Request-smuggling-style header combinations.
- [ ] Absolute-form request targets.
- [ ] Empty header values.
- [ ] Very long request lines.

### Connection behavior

- [ ] Client closes during a partial write.
- [ ] Client closes during a partial read.
- [ ] `EPIPE`.
- [ ] `ECONNRESET`.
- [x] SIGPIPE prevention (`SIG_IGN` plus `MSG_NOSIGNAL`).
- [ ] Keep-alive timeout while partially reading headers.
- [ ] Pipelined request after a response error.
- [ ] `Connection: close`.
- [ ] HTTP/1.0 behavior, if supported or deliberately rejected.
- [~] Slow-client timeout and overload tests (the benchmark has a
  `slow-clients` scenario; there is no automated regression test yet).

### Resource safety

- [ ] Maximum active connections.
- [ ] Maximum per-connection input buffer.
- [ ] Maximum output buffer.
- [ ] Maximum queued work.
- [ ] File descriptor exhaustion.
- [ ] Memory allocation failure.
- [ ] Repeated connect/disconnect cycles.
- [ ] Clients that never finish headers.
- [x] Benchmark runs with zero malformed responses and zero unexpected
  statuses.

A common C-server failure is handling the normal data path correctly but leaking
memory or file descriptors on one of these close/error paths, so those cases are
gates rather than nice-to-haves.

Performance gates should be relative to a checked-in baseline and should not
fail solely because a different machine has lower absolute capacity. Absolute
5,000 req/s acceptance belongs to the documented benchmark environment.

## Recommended Execution Order

1. [x] Add counters and structured benchmark output.
2. [ ] Pin the benchmark environment (host, CPU count, kernel, compiler flags,
   ulimit).
3. [ ] Convert the benchmark to a true fixed-rate, fixed-duration test.
4. [ ] Add warmup, steady-state, drain, and per-second reporting.
5. [~] Disable hot-path logging (done) and cache static/gzip responses
   (pending).
6. [ ] Capture a CPU/system profile and identify whether the client or the
   server is the limiter.
7. [ ] Add active-connection, buffer, timeout, and overload limits.
8. [ ] Run correctness and slow-client tests.
9. [ ] Implement nonblocking connection state with one event-loop thread.
10. [ ] Compare one event loop against the old worker model.
11. [ ] Add additional event loops or processes only if profiling justifies it.
12. [ ] Tune kernel and deployment parameters.
13. [ ] Publish a capacity report with control runs.

The success condition is not just a higher benchmark number. It is: during a
60-second steady-state window, the server completes at least 300,000 valid
requests offered at 5,000 requests/second, with zero unexpected errors,
scenario-specific p99 latency, and no unbounded growth in active connections,
memory, file descriptors, or queued work.

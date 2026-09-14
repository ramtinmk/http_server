# HTTP Server Scaling Plan

## Purpose

This document defines the path from the current educational HTTP server to a
server that can sustain 5,000 requests per second for the supported static
endpoints. The plan prioritizes measurable improvements, protocol correctness,
and predictable failure behavior over premature micro-optimizations.

The target is **sustained successful throughput**, not merely accepting 5,000
connections per second. A valid milestone must include zero request errors and
an explicit latency objective.

## Current Baseline

The current implementation has these characteristics:

- A single blocking `accept()` loop.
- A fixed pool of 16 worker threads.
- One worker handles a client socket, including keep-alive idle time.
- Blocking socket reads and writes.
- `BACKLOG` is configured as 10.
- Every accepted connection is logged synchronously with `printf()`.
- Static files are opened and stat'ed for every request.
- `sendfile()` is used for uncompressed responses.
- Gzip responses are compressed on every request.
- The benchmark defaults to a new TCP connection for every request.

The preserved 5,000 req/s stress target offered 50,000 requests. The server
completed all requests without HTTP errors after increasing the client timeout,
but the run took 26.416 seconds and achieved approximately 1,893 req/s. This
demonstrates that the server can eventually drain the workload, but does not
yet sustain the offered rate.

The first benchmark result should not be treated as a final capacity number.
It combines request processing, TCP connection setup, accept backlog behavior,
client scheduling, and queue drain time. Every future result must report both
offered load and completed throughput.

## Goals and Non-Goals

### Goals

- Sustain at least 5,000 successful HTTP requests per second on the benchmark
  host for 60 seconds.
- Maintain zero transport, framing, and HTTP-status errors during the target
  run.
- Define a latency objective of p99 below 20 ms for cached, non-gzip responses.
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

Before and after each phase, run the same benchmark matrix. Record:

- Offered request rate.
- Completed requests and failed requests.
- Achieved requests per second.
- Status-code distribution.
- p50, p95, p99, and maximum latency.
- Connection rate and keep-alive request rate.
- Server CPU utilization and RSS.
- Open file descriptors.
- Thread-pool queue depth and rejected tasks.
- System context switches and network retransmits where available.

Use separate scenarios because they exercise different bottlenecks:

| Scenario | Purpose |
| --- | --- |
| 1,000 req/s, new connection | Establish connection and accept baseline |
| 5,000 req/s, new connection | Stress accept, backlog, and connection handling |
| 5,000 req/s, keep-alive | Measure request processing without TCP setup |
| 5,000 req/s, `/home` and `/hello` mix | Avoid endpoint-specific conclusions |
| 500 req/s gzip | Measure compression CPU and chunked framing |
| Slow clients and idle keep-alive | Verify resource protection |
| 404 and unsupported methods | Measure error-path behavior |

The benchmark must distinguish a server that is slow from a client that has
finished sending load. Use response framing (`Content-Length` or chunked
encoding), never an idle read timeout, to determine request completion.

## Phase 0: Establish a Reliable Baseline

### Work

- Keep the dependency-free benchmark as the canonical load generator.
- Add a benchmark result format suitable for automation, such as JSON or CSV,
  while retaining human-readable output.
- Add server counters for accepted connections, completed requests, response
  classes, active workers, queue depth, and request failures.
- Make logging configurable and explicitly disable access logging during
  throughput measurements.
- Pin down the benchmark host, CPU count, kernel, compiler flags, and ulimit
  values in the test documentation.

### Exit criteria

- A 60-second baseline run can be repeated with less than 5% throughput
  variation.
- Every request is accounted for as successful or failed.
- A failed run identifies whether the failure was connect, send, receive,
  framing, status, or timeout related.

## Phase 1: Remove Avoidable Per-Request Work

This phase should be completed before changing the concurrency model.

### Work

- Remove unconditional `printf()` calls from the accept path, or guard them
  behind a disabled-by-default log level.
- Raise `BACKLOG` from 10 to a configurable value such as 1024, then verify the
  effective kernel limit with `somaxconn`.
- Load `home.html` and `hello.html` once at startup.
- Precompute the plain response headers and body lengths.
- Keep cached response bytes in memory and serve them directly.
- Cache gzip output for each static asset instead of running zlib per request.
- Reuse per-worker request buffers where safe.
- Avoid repeated `strlen`, `strstr`, and path formatting in the hot path.

### Risks

- Cached files can become stale if development-time file replacement is
  expected. Use an explicit reload or development mode rather than silently
  changing production semantics.
- Cached gzip data must have correct `Content-Encoding`, framing, and
  `Vary: Accept-Encoding` behavior.

### Exit criteria

- Plain cached keep-alive traffic reaches at least 5,000 req/s with zero errors
  on the benchmark host, or profiling proves the remaining limit is the
  blocking connection architecture.
- p99 latency remains below 20 ms at 5,000 req/s.
- Gzip correctness tests still pass byte-for-byte after decompression.

## Phase 2: Fix Connection and Queue Capacity

The current pool has only 16 workers, and each worker can remain blocked on an
idle client. Increasing the thread count alone is not a durable solution, but
the queue and descriptor limits must still be explicit.

### Work

- Define a bounded maximum number of queued tasks.
- Return a controlled overload response or close new connections when the queue
  is full; never allocate unbounded task memory.
- Increase the task arena and buffer pool based on measured concurrency rather
  than arbitrary constants.
- Check every `accept`, task enqueue, buffer acquisition, and send result.
- Set `FD_CLOEXEC` and document the required `RLIMIT_NOFILE`.
- Add queue-depth and active-worker instrumentation.
- Test worker counts around the available CPU count instead of assuming that
  more threads always improve throughput.

### Exit criteria

- Saturation tests do not leak file descriptors, buffers, or tasks.
- The process remains responsive when clients connect and do not send headers.
- Queue-full behavior is deterministic and visible in metrics.

## Phase 3: Move Socket I/O to an Event Loop

This is the primary architectural change if Phase 1 does not reach the target.
The current blocking worker-per-connection design cannot scale efficiently when
many persistent or slow connections exist.

### Target architecture

- One or more event-loop threads own nonblocking listening and client sockets.
- `epoll` watches accept, readable, and writable events.
- Each connection has explicit read, parse, write, and close state.
- Event loops perform lightweight HTTP parsing and response scheduling.
- A bounded worker pool handles only CPU-bound operations such as uncached
  compression or future dynamic content.
- A connection is never allowed to block an event-loop thread on disk, socket
  output, or a slow peer.

### Migration sequence

1. Introduce a connection-state structure without changing response semantics.
2. Convert accepted client sockets to nonblocking mode.
3. Implement header reads and response writes with partial-I/O handling.
4. Add per-connection output limits and write-interest registration.
5. Move keep-alive timeout handling to a monotonic timer mechanism.
6. Preserve the existing parser and ring-buffer tests while adding fragmented,
   coalesced, and pipelined socket tests.
7. Remove blocking client handling from the worker pool after parity tests pass.

### Exit criteria

- Slow-reader tests cannot stall unrelated clients.
- Keep-alive traffic uses a bounded amount of memory per connection.
- 5,000 req/s new-connection and keep-alive runs pass with zero errors.
- Event-loop CPU utilization and wakeups are measured before further tuning.

## Phase 4: Scale Accept and CPU Work

After nonblocking I/O is stable, scale only where profiling identifies a limit.

### Options

- Run multiple event-loop threads with clear ownership of connections.
- Use `SO_REUSEPORT` and multiple processes when separate accept queues improve
  distribution on the target kernel.
- Keep gzip work off the event loop and cap compression concurrency.
- Precompress known static assets during startup.
- Use `sendfile` or equivalent zero-copy output only after response caching and
  nonblocking writes are correct.

Avoid adding processes or threads solely to compensate for lock contention that
has not been measured.

## Phase 5: Operating-System and Deployment Tuning

Only apply these settings after application bottlenecks are addressed:

- Raise `RLIMIT_NOFILE` for the server and benchmark client.
- Verify `net.core.somaxconn` and TCP receive/send buffer limits.
- Use a dedicated benchmark host or isolate the server from competing CPU and
  I/O workloads.
- Record CPU frequency behavior and power-management settings.
- Consider CPU affinity only if scheduler migration is measurable.
- Monitor SYN backlog overflow, retransmits, accept errors, and listen drops.

Every tuning change must be recorded with the kernel version and reverted in a
control run to prove it improves the application rather than the environment.

## Correctness and Regression Gates

No performance optimization should merge unless these remain green:

- Ring-buffer unit tests.
- Thread-pool lifecycle and queue tests.
- Existing server endpoint tests.
- Fragmented request and fragmented response tests.
- Keep-alive and pipelining tests.
- HEAD response tests.
- 404, 400, 413, 431, and unsupported-method tests.
- Gzip decompression and chunked-framing tests.
- Slow-client timeout and overload tests.
- Benchmark runs with zero malformed responses and zero unexpected statuses.

Performance gates should be relative to a checked-in baseline and should not
fail solely because a different machine has lower absolute capacity. Absolute
5,000 req/s acceptance belongs to the documented benchmark environment.

## Recommended Execution Order

1. Add counters and structured benchmark output.
2. Disable hot-path logging and increase backlog safely.
3. Cache static responses and gzip variants.
4. Add queue, descriptor, timeout, and overload protections.
5. Re-run the benchmark matrix and profile the remaining bottleneck.
6. Implement the nonblocking event loop if blocking workers remain limiting.
7. Scale event loops or processes only after measuring accept or CPU limits.
8. Tune operating-system parameters and publish the final capacity report.

The success condition is not just a higher benchmark number. It is 5,000
successful requests per second with bounded memory, no request errors, stable
latency, and predictable behavior when the offered load exceeds capacity.

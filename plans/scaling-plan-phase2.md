# Phase 2 Implementation Plan

This plan implements Phase 2 of `plans/scaling-plan.md`: make connection,
queue, descriptor, and buffer capacity explicit and bounded while keeping the
current blocking `accept -> task queue -> worker owns socket` architecture.

Phase 2 is a resource-protection phase, not the event-loop rewrite. A worker
may still remain occupied by an idle or slow client. The implementation must
make that limitation visible and prevent overload from turning into unbounded
memory, file-descriptor, or task growth.

## Scope

Primary files are:

- `src/main.c` for accept, admission control, and connection accounting.
- `src/thread_pool.c` and `include/thread_pool.h` for bounded task and buffer
  pools and queue admission.
- `src/http_server.c` and `include/http_server.h` for socket setup, request
  limits, send/receive handling, and timeout behavior.
- `src/metrics.c` and `include/metrics.h` for capacity and overload counters.
- `tests/server_test.c`, `tests/test_ring_buffer.c`, and
  `tests/thread_pool_test.c` for regression and saturation coverage.

Do not introduce `epoll`, `poll`, `SO_REUSEPORT`, multiple accept loops, or a
new event-loop ownership model in this phase. Those belong to Phase 3 and
Phase 4 after the limits and failure behavior are measurable.

## Risks and remaining gaps

The original implementation risks that motivated this phase have been addressed
in the current code: task admission is bounded, buffer acquisition has no
uncapped fallback, enqueue reports failure, listening and accepted sockets use
`FD_CLOEXEC`, and connection/read/write overload paths have explicit handling
and metrics.

The remaining risks are narrower and still require follow-up validation:

- The task arena and buffer-pool sizes are policy defaults rather than values
  selected from a sustained-concurrency benchmark.
- Request-line and individual header-size enforcement is not yet distinct from
  aggregate input-buffer enforcement.
- Output-buffer and output-byte limits are not yet explicit.
- Active-connection and queue rejection metrics do not yet distinguish every
  rejection reason.
- Effective `somaxconn`, SYN/completed-connection queue behavior, listen drops,
  and errno-specific accept/send metrics are not yet measured.
- Monotonic deadline tracking and long-running descriptor/RSS leak campaigns
  remain outstanding.

The plan continues to preserve normal HTTP behavior while making every
implemented overload path bounded, deterministic, and observable.

## Configuration contract

Add named configuration values with safe defaults and environment or compile-
time overrides only where the existing project conventions support them. Keep
all values in one documented configuration section rather than scattering
magic numbers across the server.

The initial values should be selected from measured benchmark concurrency, not
from the 5,000 req/s target alone. Record the selected values in startup output
and the metrics snapshot so a benchmark result can be reproduced.

At minimum define:

- Worker count.
- Maximum queued tasks.
- Maximum active connections.
- Task-arena capacity and buffer-pool capacity.
- Maximum request-line bytes.
- Maximum header count and aggregate header bytes.
- Maximum per-connection input-buffer bytes.
- Maximum per-connection output-buffer bytes and total output bytes.
- Maximum keep-alive requests per connection.
- Header-read timeout, idle keep-alive timeout, and write timeout.
- Maximum buffered pipelined requests.
- Required `RLIMIT_NOFILE` headroom.

Document whether each limit is inclusive, which HTTP status or close behavior
it produces, and whether it applies per connection or process-wide.

## Implementation status

Phase 2 is partially implemented. The current code has completed the core
bounded-admission changes while preserving the blocking worker-per-connection
architecture. The implementation has been built successfully and the ring,
thread-pool, and server integration suites pass, including the new Phase 2
coverage for queue configuration, strict buffer-pool exhaustion, header
timeouts, keep-alive request limits, and oversized input.

Implemented files include `include/server_config.h`, `src/main.c`,
`src/http_server.c`, `src/thread_pool.c`, `src/metrics.c`, and their related
headers and tests.

Implemented defaults are:

| Limit | Default |
| --- | ---: |
| Worker threads | 16 |
| Queued tasks | 256 |
| Active connections | 512 |
| Input buffer | 64 KiB |
| Pipeline depth | 16 |
| Keep-alive requests | 100 |
| Initial header timeout | 5 s |
| Idle keep-alive timeout | 30 s |
| Write timeout | 10 s |

The current overload policy closes newly accepted sockets when active-connection
admission, task allocation, or queue admission fails. Buffer-pool exhaustion
also closes the connection without allocating an uncapped fallback buffer.
Configured limits are printed at startup, `RLIMIT_NOFILE` is reported, and
active-connection/overload counters are included in the metrics snapshot.

Remaining work is listed explicitly below; in particular, saturation/leak
campaigns, kernel backlog measurements, worker-count experiments, and several
fine-grained protocol/output limits are not complete.

## Implementation sequence

### 1. Establish explicit accounting and configuration

1. [~] Add a configuration structure or constants for the limits above, with
   compile-time overrides and startup reporting. Positive-value and
   cross-limit validation, plus metrics-file publication of configured limits,
   remain.
2. [x] Track process-wide active connections and queued tasks using atomics for
   metrics and a mutex-protected queue length; do not infer active connections
   from worker count.
3. [~] Track per-connection request count and input size. Monotonic activity
   timestamps and explicit output-byte accounting remain.
4. [~] Include current/maxima values and overload counters in metrics snapshots.
   Configured-limit fields and effective kernel/backlog values remain.
5. [ ] Make startup fail clearly for invalid or contradictory configuration.

### 2. Make task admission bounded

1. [~] Change task enqueue to return an explicit success/failure result. The
   current API returns `0` or `-1`; reason-specific return codes remain.
2. [x] Check queue length under `queue_mutex`; the queue cannot exceed
   `MAX_QUEUED_TASKS`.
3. [x] Return an allocated task to the arena on queue-full or shutdown
   rejection.
4. [x] On queue or task-arena exhaustion, count the rejection, close the
   just-accepted socket in the accept loop, and continue accepting.
5. [x] Remove the previous unconditional task-exhaustion log from the hot
   rejection path; metrics are the primary signal.
6. [~] Shutdown closes queued sockets exactly once; explicit task-arena return
   verification and a dedicated shutdown/leak test remain.

The first Phase 2 behavior should be deterministic close-on-overload. An HTTP
503 response is optional only if it can be sent without blocking and without
letting overload clients consume the same scarce queue capacity.

### 3. Bound buffer acquisition and request growth

1. [x] `buffer_acquire()` returns `NULL` when the bounded pool is exhausted;
   there is no fallback allocation.
2. [x] The worker releases the acquired buffer on parse, read, timeout, send,
   and close paths through the connection cleanup path.
3. [~] Enforce aggregate input capacity before ring-buffer growth. The 64 KiB
   aggregate cap is implemented, but request-line and individual header-size
   enforcement still need explicit protocol checks and 431 coverage.
4. [~] Apply `SO_SNDTIMEO` and close slow readers. Explicit output-buffer and
   per-connection output-byte caps remain.
5. [x] Cap processing of pipelined requests and cap keep-alive requests per
   connection while preserving response order.
6. [x] Return buffers through the existing pool and trim excessively large ring
   buffers before reuse.

### 4. Add connection admission and timeout enforcement

1. [x] Compare active connections with `MAX_ACTIVE_CONNECTIONS`; count and
   close immediately when admission is full.
2. [~] Increment/decrement active-connection accounting around admission and
   worker close. The normal paths are covered; defensive double-release
   protection remains.
3. [x] Apply the 5-second initial header timeout and 30-second idle keep-alive
   timeout.
4. [x] Apply a 10-second `SO_SNDTIMEO` write timeout and count write timeouts.
5. [ ] Use monotonic deadlines and activity timestamps; the current policy uses
   socket timeouts rather than explicit monotonic deadline tracking.
6. [~] Treat common disconnects and timeout errors as expected closes. Complete
   errno-specific accept/send metrics remain.
7. [x] A client that sends no headers is closed after the bounded header timeout
   and covered by an integration test.

The implementation may use the existing blocking model and socket timeouts in
this phase, but timeout setup and error handling must be checked and reported.
Avoid making sockets nonblocking as a partial event-loop migration.

### 5. Harden socket and descriptor handling

1. [~] Check socket setup, `bind`, `listen`, `accept`, task admission, buffer
   acquisition, and send results. Core return paths are checked; detailed
   errno classification and `fcntl` failure handling remain.
2. [x] Set `FD_CLOEXEC` on the listening socket and accepted sockets, using
   `accept4(..., SOCK_CLOEXEC)` where available.
3. [~] Report `RLIMIT_NOFILE` and required headroom at startup. Enforcing the
   limit, raising it automatically, and documenting deployment commands remain.
4. [x] Preserve `SO_REUSEADDR`; `SO_REUSEPORT` remains deferred.
5. [~] Report configured backlog and descriptor limits. Effective `somaxconn`,
   SYN backlog, completed-connection queue behavior, and listen drops remain.
6. [~] Continue after transient accept errors. Backoff/rate limiting and
   dedicated accept-error metrics remain.

## Metrics and observability

The metrics snapshot now includes active-connection current/high-water values
and counters for admission rejection, buffer-pool exhaustion, header timeout,
idle timeout, write timeout, and input-buffer limits. Queue depth, worker
activity, rejected tasks, response classes, and request failures remain
available.

The following observability work is still pending: configured-limit fields in
metrics snapshots, per-reason queue rejection fields, errno-specific accept and
send counters, and effective kernel backlog values.

The remaining target contract is:

- Current and maximum active connections.
- Current and maximum queued tasks.
- Queue-full, task-arena, buffer-pool, and active-connection rejections.
- Request-line, header, input-buffer, output-buffer, and pipeline-limit
  violations.
- Header-read, idle, and write timeouts.
- Accept failures by errno category.
- Send failures by errno category.
- Current configured limits and effective file-descriptor/backlog values.
- Connections closed by overload, timeout, client disconnect, and server
  shutdown.

Metrics updates must not allocate memory or block on slow I/O. Preserve the
existing structured metrics output so the benchmark can compare before/after
runs and identify whether the client or server limited throughput.

## Test plan

### Unit tests

Implemented focused coverage includes queue configuration and success return,
plus strict buffer-pool exhaustion. Add the remaining cases below:

- Queue admission at zero, one, and maximum capacity.
- Queue-full and task-arena exhaustion returning deterministic statuses.
- Task reuse after rejection and after normal worker completion.
- Buffer-pool exhaustion returning `NULL` without allocation growth.
- Buffer release after every error path and oversized-buffer trimming.
- Active-connection accounting under successful close, timeout, and failed
  enqueue paths.
- Configuration validation and limit reporting.

### Server integration tests

Implemented coverage includes clients that send no headers, keep-alive request
limits, and oversized aggregate input. Add the remaining cases below:

- More simultaneous connections than the active-connection limit.
- More queued tasks than the queue limit.
- Clients that connect but never complete headers.
- Fragmented headers exceeding request-line or aggregate-header limits.
- Too many headers and overlong individual header values.
- Slow readers that exceed the write policy.
- Keep-alive idle timeout and maximum requests per connection.
- Pipelining up to the limit and deterministic behavior above it.
- Client close during partial read and partial write, including `EPIPE` and
  `ECONNRESET` where reproducible.
- Repeated connect/disconnect cycles with stable descriptor and RSS counts.
- File-descriptor exhaustion or a reduced `RLIMIT_NOFILE` test environment.
- Graceful shutdown with queued and active sockets, verifying no descriptor or
  task is leaked.

Existing successful cases must remain green: plain and gzip responses, `HEAD`,
404, unsupported methods, keep-alive, and pipelining.

## Benchmark and acceptance validation

Run validation from the repository root using the documented binaries in
`bin/`:

1. Run `make` and the ring-buffer and thread-pool suites.
2. Run the server integration suite with the server started from the
   repository root.
3. Run the `slow-clients` and `error-paths` benchmark scenarios.
4. Run `new-connection-1000`, `new-connection-5000`, and
   `keep-alive-5000` with the same warmup, steady-state, and drain contract
   used by `plans/scaling-plan.md`.
5. Compare throughput, p99 latency, error categories, active connections,
   queue depth, rejections, RSS, and open descriptors against the Phase 1
   baseline.
6. Repeat saturation tests long enough to show that active connections,
   queued tasks, buffers, file descriptors, and RSS return to baseline after
   clients disconnect.
7. Test worker counts around available CPU count, but keep the selected value
   based on measurements rather than assuming more workers improve throughput.

Phase 2 should not be declared complete based only on a higher throughput
number. The resource limits and overload counters must be visible in the
metrics output, and the slow-client tests must demonstrate bounded behavior.

## Exit criteria

Phase 2 is currently **partial**, not complete. The core admission and timeout
protections are implemented, but the remaining criteria below require targeted
saturation, leak, and kernel-limit validation.

Phase 2 is complete when all of the following are true:

- Queue, task, buffer, connection, request, output, pipeline, and timeout
  limits are configured, enforced, and observable.
- Queue-full, pool-exhaustion, and active-connection overload behavior is
  deterministic and does not block the accept loop indefinitely.
- Clients that do not send headers cannot consume a worker without a bounded
  timeout.
- Saturation and repeated connect/disconnect tests show no task, buffer, file
  descriptor, or memory leak.
- `FD_CLOEXEC` and the required `RLIMIT_NOFILE` are documented and verified.
- Existing HTTP correctness tests pass, including keep-alive and pipelining.
- The benchmark reports server-side limiting behavior and stable resource
  maxima; no resource grows without bound during warmup, steady state, or
  drain.
- The Phase 2 changes do not regress the Phase 1 cached response path.

The 5,000 req/s sustained acceptance target remains a broader scaling-plan
gate. If the blocking worker-per-connection architecture still fails that
target after these protections are in place, record the profile and proceed to
Phase 3 rather than increasing limits without evidence.

## Rollback and operational notes

Keep each limit behind the configuration contract so a benchmark can compare
control and treatment runs. If a limit causes unexpected behavior, first
restore the previous value while retaining metrics and tests; do not remove
admission checks or revert to unbounded allocation. Record the configured
limits, effective kernel limits, worker count, and benchmark host with every
result.

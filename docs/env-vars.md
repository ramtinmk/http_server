# Environment Variables and Limits

Two families of knobs:

1. **Runtime environment variables** read with `getenv()` while the server runs.
2. **Compile-time limits** — `#define`s in `include/server_config.h` and
   `include/http_server.h`, each wrapped in `#ifndef`. These are *not* shell
   variables; see [Overriding compile-time limits](#overriding-compile-time-limits).

`include/server_config.h` is the single source of truth. If you add a limit,
add it there (and here) rather than sprinkling magic numbers.

## Runtime environment variables

| Variable                       | Default                              | Effect |
|--------------------------------|--------------------------------------|--------|
| `HTTP_SERVER_METRICS_FILE`     | unset                                | Path for periodic single-line JSON snapshots. Starts a detached reporter thread at a 250 ms cadence that atomically rewrites the file. See `src/main.c:492`. |
| `HTTP_SERVER_MAX_CONNECTIONS`  | `MAX_ACTIVE_CONNECTIONS` (1024)      | Operator cap on simultaneously active connections. Effective capacity is `min(this, rlimit-derived, EL_MAX_CONNECTION_TABLE)`; a non-positive or malformed value is fatal. See `src/main.c:423`. |
| `HTTP_SERVER_CPU_SET`          | unset                                | `taskset(1)`-style CPU list (`0-3`, `0,2,4`) that pins the process via `sched_setaffinity`. Unset means no pinning. Invalid syntax/range is fatal. See `src/main.c:331`. |
| `HTTP_SERVER_MALLOC_ARENA_MAX` | `MALLOC_ARENA_MAX_DEFAULT` (2)       | glibc arena cap applied with `mallopt(M_ARENA_MAX, …)` before threads start. `1` minimizes VmSize but serializes allocations; a non-positive/malformed value is fatal. Ignored when `MALLOC_ARENA_MAX` is preset. See `src/main.c:59`. |
| `MALLOC_ARENA_MAX`             | unset                                | glibc's own preset. When set, the server respects it and does not call `mallopt`. See `src/main.c:51`. |
| `HTTP_SERVER_ACCESS_LOG`       | unset                                | **Currently a no-op.** The benchmark scripts set it to `0`, but no code in `src/` reads it. Do not rely on it to silence logging (there is no access-log path). |

## Compile-time limits

All values live in `include/server_config.h` unless noted.

| Macro                        | Default | Meaning |
|------------------------------|--------:|---------|
| `PORT` *(http_server.h)*     | 8081    | Listen port. |
| `BACKLOG` *(http_server.h)*  | 1024    | `listen(2)` backlog; the effective backlog is clamped by `net.core.somaxconn`. |
| `MAX_ACTIVE_CONNECTIONS`     | 1024    | Maximum simultaneously open connections. |
| `MAX_KEEPALIVE_REQUESTS`     | 100     | Requests per keep-alive connection before the server forces close. |
| `MAX_INPUT_BUFFER_BYTES`     | 65536   | Per-connection buffered request+header cap; exceeded → `413`. |
| `MAX_PIPELINE_DEPTH`         | 16      | Pipelined requests processed from one `recv()` pass before a new read. |
| `HEADER_READ_TIMEOUT_SEC`    | 5       | Time to deliver complete request headers after accept. |
| `IDLE_TIMEOUT_SEC`           | 30      | Keep-alive idle time between requests. |
| `WRITE_TIMEOUT_SEC`          | 10      | Time to drain a full response to the socket. |
| `REQUIRED_NOFILE_HEADROOM`   | 64      | Non-connection descriptors reserved above capacity. |
| `REQUIRED_NOFILE_PER_LOOP`   | 3       | Extra descriptors per loop (epoll fd, wake eventfd, listener). |
| `EL_THREAD_COUNT`            | 0       | Event-loop count. `0` = one per online core; positive = explicit override; `1` = single-loop control. **Compile-time only.** |
| `EL_MAX_THREADS`             | 64      | Upper bound / clamp on the loop count. |
| `EL_MAX_EVENTS`              | 256     | Max events per `epoll_wait`. |
| `EL_DEADLINE_SCAN_MS`        | 100     | Deadline-scanner interval (ms). |
| `EL_MAX_CONNECTION_TABLE`    | 65536   | Hard upper bound on the runtime connection table. |
| `MALLOC_ARENA_MAX_DEFAULT`   | 2       | Default glibc arena cap when no env override/preset. |

### Overriding compile-time limits

The server-side limits are preprocessor macros; setting `MAX_ACTIVE_CONNECTIONS=256`
in your shell does nothing. Pass them to the compiler through CMake, then
rebuild:

```bash
cmake -S . -B . -DCMAKE_C_FLAGS="-DEL_THREAD_COUNT=4 -DMAX_ACTIVE_CONNECTIONS=256"
make
```

Because every definition is `#ifndef`-guarded, `-D` wins over the header
default. The resolved values are printed at startup by `print_server_config()`
so benchmark runs stay reproducible.

> `EL_THREAD_COUNT` is the common trap: docs and plans write it like an
> environment variable, but it is only honored at compile time. See
> `docs/gotchas.md`.

## Benchmark-harness variables

Read by `scripts/run_benchmark_pinned.sh` and `scripts/run_benchmark_matrix.sh`,
not by the server.

| Variable               | Default                  | Effect |
|------------------------|--------------------------|--------|
| `HPIN_SERVER_CPUS`     | first half of online CPUs | CPU set for the server. |
| `HPIN_CLIENT_CPUS`     | second half              | CPU set for the load generator; must be disjoint from the server set. |
| `HPIN_REQUIRE_GOVERNOR`| `0`                      | `1` makes the preflight fail unless the `performance` governor is active. |
| `BENCH_SERVER_CPUS`    | exported by the pinned runner | Propagated to `run_benchmark_matrix.sh` as `--server-cpus`. |
| `BENCH_CLIENT_CPUS`    | exported by the pinned runner | Propagated as `--client-cpus`. |

# Environment Variables and Limits

Three layers, resolved in precedence order
**defaults < config file < environment < command line**:

1. **Config file** — `key = value` lines, selected by `HTTP_SERVER_CONFIG` or
   `--config <path>`. Unknown keys are fatal and name the key.
2. **Runtime environment variables** — read with `getenv()` at startup. Each key
   below has a matching `--<key>` / `--<key>=value` command-line flag.
3. **Compile-time limits** — `#define`s in `include/server_config.h` and
   `include/http_server.h`, each wrapped in `#ifndef`. These are *not* shell
   variables; see [Overriding compile-time limits](#overriding-compile-time-limits).

`include/config.h` / `src/config.c` own the runtime surface;
`include/server_config.h` owns the defaults. If you add a runtime knob, add a
row to `CFG_KEYS` in `src/config.c`, a default in `config_defaults()`, and an
`ENV_*` name in `include/server_config.h`, then document it here.

## Runtime configuration

Keys may be written with `-` or `_` on the command line. A malformed or
out-of-range value is fatal and names the exact key (for example
`value 0 for key 'idle_timeout' is out of range 1..86400`).

| Key / variable                     | Default                              | Effect |
|------------------------------------|--------------------------------------|--------|
| `config` / `HTTP_SERVER_CONFIG`    | unset                                | Config file path. |
| `port` / `HTTP_SERVER_PORT`        | `PORT` (8081)                        | Listen port (1..65535). |
| `backlog` / `HTTP_SERVER_BACKLOG`  | `BACKLOG` (1024)                     | `listen(2)` backlog; effective value clamped by `net.core.somaxconn`. |
| `max_connections` / `HTTP_SERVER_MAX_CONNECTIONS` | `MAX_ACTIVE_CONNECTIONS` (1024) | Operator cap on active connections; effective capacity is `min(this, rlimit-derived, EL_MAX_CONNECTION_TABLE)`. |
| `max_keepalive_requests` / `HTTP_SERVER_MAX_KEEPALIVE_REQUESTS` | `MAX_KEEPALIVE_REQUESTS` (100) | Requests per connection before the server forces `Connection: close`. |
| `max_input_buffer_bytes` / `HTTP_SERVER_MAX_INPUT_BUFFER_BYTES` | `MAX_INPUT_BUFFER_BYTES` (65536) | Per-connection buffered request cap; exceeded → `413`. |
| `header_read_timeout` / `HTTP_SERVER_HEADER_READ_TIMEOUT` | `HEADER_READ_TIMEOUT_SEC` (5) | Seconds to deliver complete headers after accept. |
| `idle_timeout` / `HTTP_SERVER_IDLE_TIMEOUT` | `IDLE_TIMEOUT_SEC` (30) | Keep-alive idle seconds between requests. |
| `write_timeout` / `HTTP_SERVER_WRITE_TIMEOUT` | `WRITE_TIMEOUT_SEC` (10) | Seconds to drain a full response. |
| `shutdown_drain_timeout` / `HTTP_SERVER_SHUTDOWN_DRAIN_TIMEOUT` | `SHUTDOWN_DRAIN_TIMEOUT_SEC` (10) | Seconds `SIGTERM`/`SIGINT` may spend draining in-flight responses. |
| `log_level` / `HTTP_SERVER_LOG_LEVEL` | `info` | `error`, `warn`, `info`, or `debug`. |
| `log_file` / `HTTP_SERVER_LOG_FILE` | unset (stderr) | JSON log target. `SIGHUP` reopens it (for logrotate). |
| `access_log` / `HTTP_SERVER_ACCESS_LOG` | `0` | Emit one JSON access record per completed response. |
| `document_root` / `HTTP_SERVER_DOCUMENT_ROOT` | `DOCUMENT_ROOT` (`.`) | Directory every non-alias request path is resolved beneath. Relative paths resolve against the working directory. |
| `index_files` / `HTTP_SERVER_INDEX_FILES` | `INDEX_FILES` (`index.html,index.htm`) | Comma-separated index file names tried for a directory; no directory listing. |
| `mime_types` / `HTTP_SERVER_MIME_TYPES` | unset (builtin) | Optional `mime.types`-style file; entries override the builtin extension map. |
| `hidden_files` / `HTTP_SERVER_HIDDEN_FILES` | `0` | When `1`, serve dotfiles; when `0` a leading-dot segment is `403`. |
| `symlinks` / `HTTP_SERVER_SYMLINKS` | `0` | When `1`, follow symlinks that stay beneath the root (requires `openat2`); when `0`, any symlink is refused. |
| `cache_budget_bytes` / `HTTP_SERVER_CACHE_BUDGET_BYTES` | `CACHE_BUDGET_BYTES_DEFAULT` (16 MiB) | Total bytes the representation cache may hold; `0` disables caching and streams every file. |

### Operational variables

| Variable                       | Default                              | Effect |
|--------------------------------|--------------------------------------|--------|
| `HTTP_SERVER_METRICS_FILE`     | unset                                | Path for periodic single-line JSON snapshots (250 ms cadence, atomic rewrite). |
| `HTTP_SERVER_CPU_SET`          | unset                                | `taskset(1)`-style CPU list (`0-3`, `0,2,4`) applied with `sched_setaffinity`. Invalid syntax/range is fatal. |
| `HTTP_SERVER_MALLOC_ARENA_MAX` | `MALLOC_ARENA_MAX_DEFAULT` (2)       | glibc arena cap applied before threads start. Ignored when `MALLOC_ARENA_MAX` is preset. |
| `MALLOC_ARENA_MAX`             | unset                                | glibc's own preset; when set the server respects it. |
| `NOTIFY_SOCKET`                | unset                                | When set, the server sends `READY=1` after binding and `STOPPING=1` on shutdown (systemd `Type=notify`). |

Command-line flags mirror the key names plus `--config` and `--help`; for
example `--max-keepalive-requests 10 --idle-timeout 5`. `EL_THREAD_COUNT` is
*not* runtime-tunable — it stays compile-time by convention.

## Compile-time limits

All values live in `include/server_config.h` unless noted. Values that also
appear as runtime keys above are the **defaults**; a runtime value overrides
the compiled default without a rebuild.

| Macro                        | Default | Meaning |
|------------------------------|--------:|---------|
| `PORT` *(http_server.h)*     | 8081    | Listen port. |
| `BACKLOG` *(http_server.h)*  | 1024    | `listen(2)` backlog; the effective backlog is clamped by `net.core.somaxconn`. |
| `MAX_ACTIVE_CONNECTIONS`     | 1024    | Maximum simultaneously open connections. |
| `MAX_KEEPALIVE_REQUESTS`     | 100     | Requests per keep-alive connection before the server forces close. |
| `MAX_INPUT_BUFFER_BYTES`     | 65536   | Per-connection buffered request+header cap; exceeded → `413`. |
| `MAX_PIPELINE_DEPTH`         | 16      | Pipelined requests processed from one `recv()` pass before a new read. |
| `MAX_MULTIPART_RANGES`       | 8       | Max satisfiable ranges in one `multipart/byteranges` `206`; more → full `200`. |
| `MAX_MULTIPART_BYTES`        | 8192    | Max assembled `multipart/byteranges` body size; larger → full `200`. |
| `DOCUMENT_ROOT` *(default)*  | `.`     | Default `document_root` runtime value. |
| `INDEX_FILES` *(default)*    | `index.html,index.htm` | Default `index_files` runtime value. |
| `CACHE_MAX_FILE_BYTES`       | 1048576 | Largest file read into the representation cache; larger files stream via `sendfile` (identity only). |
| `CACHE_MAX_ENTRIES`          | 256     | Hard cap on cache entries independent of the byte budget. |
| `CACHE_BUDGET_BYTES_DEFAULT` | 16777216 | Default `cache_budget_bytes` runtime value (16 MiB); runtime `0` disables caching. |
| `HEADER_READ_TIMEOUT_SEC`    | 5       | Time to deliver complete request headers after accept. |
| `IDLE_TIMEOUT_SEC`           | 30      | Keep-alive idle time between requests. |
| `WRITE_TIMEOUT_SEC`          | 10      | Time to drain a full response to the socket. |
| `SHUTDOWN_DRAIN_TIMEOUT_SEC` | 10      | Default drain deadline on `SIGTERM`/`SIGINT`. |
| `LOG_POLL_INTERVAL_MS`       | 200     | Log-writer poll tick (ms); bounds `SIGHUP` reopen latency. |
| `REQUIRED_NOFILE_HEADROOM`   | 64      | Non-connection descriptors reserved above capacity. |
| `REQUIRED_NOFILE_PER_LOOP`   | 3       | Extra descriptors per loop (epoll fd, wake eventfd, listener). |
| `EL_THREAD_COUNT`            | 0       | Event-loop count. `0` = one per online core; positive = explicit override; `1` = single-loop control. **Compile-time only.** |
| `EL_MAX_THREADS`             | 64      | Upper bound / clamp on the loop count. |
| `EL_MAX_EVENTS`              | 256     | Max events per `epoll_wait`. |
| `EL_ACCEPT_BATCH_SIZE`       | 64      | Maximum accepted sockets handled per listener event dispatch; bounds overload rejection work before the loop services other events. |
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

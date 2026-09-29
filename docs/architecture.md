# Architecture

C11 static-file HTTP/1.1 server. One process, nonblocking `epoll` event loops,
no external runtime dependencies beyond libc, pthreads, and zlib. This document
is the mental model: what each module owns, how a request flows, and which
invariants must hold when you change the code.

## Process model

- `src/main.c` runs the single-threaded startup path, then hands the bound
  listener to `event_loop_run()`.
- `event_loop_run()` starts **one event-loop thread per resolved loop count**.
  The loop count is `EL_THREAD_COUNT` when it is positive (compile-time), else
  `sysconf(_SC_NPROCESSORS_ONLN)` clamped to `[1, EL_MAX_THREADS]`
  (`event_loop_thread_count()`, `src/event_loop.c:976`).
- When there is more than one loop, every loop binds its own `SO_REUSEPORT`
  listener on the same port and the kernel hashes new connections across them.
  Each loop **exclusively owns** the connections it accepts: its epoll set,
  connection table, free list, deadline scan, and wake `eventfd`
  (`include/event_loop.h:63`). No connection state is shared between loops.
- Shared between loops: the process-wide active-connection gauge and admission
  counter in `src/metrics.c` (relaxed atomics / CAS). Capacity admission is
  enforced atomically across loops via `metrics_connection_admit()`.
- The metrics reporter is a separate detached thread that periodically writes a
  JSON snapshot. It is the only thread that samples memory (`memory_profiler`).

## Module map

| File                        | Owns                                                                 |
|-----------------------------|----------------------------------------------------------------------|
| `src/main.c`                | Socket setup, allocator cap, startup preflight, signal handling, effective capacity, loop-count resolution, dispatch selection |
| `src/config.c`              | Single validated configuration surface (defaults < file < env < CLI), range/unknown-key rejection, usage text |
| `src/event_loop.c`          | Nonblocking per-loop connection state machine, pipeline queue, deadlines, admission/overload handling, graceful drain, listen-drop sampling |
| `src/http_server.c`         | Socket creation, HTTP/1.1 parsing, static asset + gzip caching, response selection |
| `src/log.c`                 | Leveled JSON access/error logging; nonblocking pipe + writer thread; `SIGHUP` reopen |
| `src/sd_notify.c`           | Dependency-free `sd_notify` (`READY`/`STOPPING`) over `$NOTIFY_SOCKET` |
| `src/ring_buffer.c`         | Bounded circular byte buffer and line reader with rollback           |
| `src/metrics.c`             | Lock-free counters, JSON snapshot rendering, reporter thread         |
| `src/memory_profiler.c`     | `/proc` + `mallinfo2()` sampling (reporter thread only)              |
| `include/config.h`          | Runtime-tunable `ServerConfig` and its log-level enum                |
| `include/server_config.h`   | Every compile-time limit and default, and the runtime `ENV_*` names   |

## Configuration

- `config_defaults()` seeds `ServerConfig` from `server_config.h`; then the
  config file (`HTTP_SERVER_CONFIG` / `--config`), environment, and CLI are
  applied in that order, each overriding the previous.
- A single `CFG_KEYS` table in `src/config.c` maps each key to its struct
  offset, type, range, and environment name. An unknown key or an
  out-of-range value is fatal and names the offending key; `--help` prints the
  surface. The resolved values are printed at startup and recorded as a
  structured `effective_config` log record.
- `max_connections`, `max_keepalive_requests`, `max_input_buffer_bytes`, the
  three timeouts, and the drain timeout are read once and stored in the
  `EventLoop`, so the hot path reads a struct field rather than a macro.
  `EL_THREAD_COUNT` remains compile-time by convention.

## Lifecycle and shutdown

- Startup order: config load → allocator cap → logging → metrics reporter →
  signal handlers → `print_server_config()` → loop-count resolution → FD
  preflight → effective capacity → host report → validation → affinity →
  static assets → bind/listen → `sd_notify(READY)` → event loops.
- `SIGINT`/`SIGTERM` clear the `server_running` flag. Each loop then removes
  its listener from epoll (stop accepting) and keeps servicing connections until
  there is no work or `shutdown_drain_timeout_sec` elapses; buffered requests
  are still parsed and answered so nothing is truncated. `el_writable()` closes a
  connection instead of returning it to idle keep-alive once the drain has
  begun. Remaining connections are force-closed at the deadline.
- `SIGHUP` does not reload configuration; it asks the log writer to reopen its
  file. `sd_notify(STOPPING)` is emitted during shutdown.


## Startup sequence (`src/main.c`)

1. `config_defaults()` + `config_load()` — resolve the single validated
   configuration surface (file, environment, CLI); invalid values are fatal.
2. `configure_allocator()` — cap glibc arenas (`mallopt(M_ARENA_MAX, …)`)
   **before any thread exists**, so all threads inherit it.
3. `log_init()` opens the log target and starts the writer thread.
4. Read `HTTP_SERVER_METRICS_FILE`; start the reporter thread if set.
5. Install `SIGINT`/`SIGTERM` handlers (no `SA_RESTART`, so a blocked accept
   unblocks) and ignore `SIGPIPE`; `SIGHUP` asks the log writer to reopen.
6. `print_server_config()` prints and logs every resolved limit.
7. Resolve the loop count, then enforce `RLIMIT_NOFILE ≥ max_connections +
   REQUIRED_NOFILE_HEADROOM + REQUIRED_NOFILE_PER_LOOP × loops`; failure is
   fatal.
8. Derive the **effective connection capacity**:
   `min(operator max, rlimit-derived, EL_MAX_CONNECTION_TABLE)`; prints each
   input, clamps with a warning, and fails on non-positive.
9. Report host sysctls (`somaxconn`, `tcp_rmem`/`tcp_wmem`) read-only.
10. `validate_configuration()`, `apply_cpu_affinity()` (`HTTP_SERVER_CPU_SET`),
    `initialize_static_responses()`, then bind/listen, `sd_notify(READY)`, and
    enter the loops.

## Connection lifecycle

`ELConnState` (`include/event_loop.h:12`):

```
CONN_READING_HEADERS --complete request--> (enqueue response) --+
          ^                                                       |
          |                                                       v
          |                                              CONN_WRITING
          |                                                       |
          +---------------- response drained <-------------------+
                    (idle deadline)              (keep-alive)
```

- `process_input()` (`src/event_loop.c:371`) pulls complete requests from the
  loop-owned `RingBuffer` and calls `el_prepare_response()` for each, up to
  `MAX_PIPELINE_DEPTH` per read pass. Responses are queued in the connection's
  `PendingResponse pq[]` ring.
- `el_prepare_response()` (`src/http_server.c:427`) is **transactional**: on an
  incomplete request it restores the ring buffer's `tail`/`size` and returns 1
  so the loop buffers more bytes. On success it fills a descriptor whose
  pointers reference **cached/static memory**.
- `el_writable()` sends header then body through `send_slice()`; when the queue
  drains the connection returns to keep-alive and its deadline is reset.
- `el_scan_deadlines()` runs every `EL_DEADLINE_SCAN_MS` and closes connections
  past their monotonic deadline (header-read, idle, or write timeout).
- `conn_close()` removes the fd from epoll, releases the buffer, returns the
  slot to the loop pool, and decrements the shared active gauge.

Close reasons are enumerated in `ELCloseReason` (`include/event_loop.h:21`).

## Admission and overload

- A new connection is admitted only if the process-wide active gauge is below
  the effective capacity (atomic). When a loop's own connection table is full,
  the reject reason is `ADMISSION_REJECT_TABLE_FULL`; the global cap yields
  `ADMISSION_REJECT_CAPACITY` (`include/metrics.h:71`).
- Each listener dispatch accepts at most `EL_ACCEPT_BATCH_SIZE` sockets. The
  limit gives other events a chance to run under a connection flood; level-
  triggered epoll will schedule another listener pass while connections remain
  queued.
- At capacity, accepted sockets do not consume a connection-table slot. The
  loop attempts a complete nonblocking `503`. After a full response it consumes
  at most 4 KiB of pending input and closes; if the response cannot be sent in
  full it resets the socket promptly. No listener-disable starvation occurs;
  brief kernel accept-queue occupancy is serviced in later
  bounded batches. If the process cannot accept because descriptors are
  exhausted, kernel backlog limits and TCP timeout/refusal behavior still
  apply.

## Static assets and gzip

- At startup `initialize_static_responses()` reads `home.html` and `hello.html`
  **by relative path** and precompresses both with zlib. This is why the server
  must run from the repository root, and why a missing asset makes startup fail.
- Each asset stores a plain and gzip variant, and two header blocks
  (keep-alive / close). `el_prepare_response()` picks gzip only when the client
  sends an acceptable `Accept-Encoding: gzip` (`gzip_is_accepted()`), including
  `q=0` refusal.
- Routes (`src/http_server.c:493`): `/` and `/home` → `home.html`,
  `/hello` → `hello.html`, anything else → `404`. Methods are `GET` and `HEAD`
  only; others → `501`.

## Instrumentation

- `src/metrics.c` counters are relaxed atomics updated on the hot path.
- `metrics_snapshot()` renders one single-line JSON object and appends the
  memory-profiler fields via `memory_profiler_append_json()`.
- `src/memory_profiler.c` samples `/proc/self/statm`,
  `/proc/self/smaps_rollup`, and `mallinfo2()` (falling back to `mallinfo()`),
  updating high-water marks. Unavailable sources contribute zero and are
  reflected by `memory_sample_ok`. **Only the reporter thread calls
  `memory_profiler_sample()`**; the data path never touches procfs or the
  allocator accounting.
- `src/log.c` formats JSON lines and writes them to an `O_NONBLOCK` pipe; a
  dedicated writer thread drains the pipe to the log file (or stderr). A full
  pipe drops the record and bumps `dropped_logs`; the event loop never blocks on
  logging. `log_access()` fires once per completed response in `el_writable()`.

## Invariants (do not break these)

1. A connection is owned by exactly one loop; never touch another loop's
   `ELConnection`.
2. `PendingResponse.header`/`body` point into cached or static memory; the
   event loop must never `free()` them.
3. `el_prepare_response()` must leave the ring buffer unchanged when it returns
   incomplete, or fragmented requests corrupt pipelining.
4. The allocator cap is set before any thread starts.
5. Admission is atomic and process-wide; the per-loop table is only an upper
   bound, not the cap.
6. The reporter thread is the sole caller of memory sampling.
7. Runtime limits are resolved once into `EventLoop.config` before any loop
   thread starts; the hot path never reads the environment or a config file.
8. A log record is written with a single pipe write of at most `PIPE_BUF`
   bytes, so records from different loops never interleave; producers never
   block (drops, not stalls).

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
  When TLS is enabled each loop also binds a `SO_REUSEPORT` listener on
  `tls_port`. Each loop **exclusively owns** the connections it accepts: its
  epoll set, connection table, free list, deadline scan, and wake `eventfd`
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
| `src/http_server.c`         | Socket creation, HTTP/1.1 parsing, static asset + gzip caching, document-root response selection |
| `src/tls.c`                 | OpenSSL context/policy (TLS 1.2+, ALPN `http/1.1`), cert/key load, permission check, `SIGHUP` reload |
| `src/path_resolver.c`       | Safe document-root path resolution (percent-decode, normalize, `openat2`/`O_NOFOLLOW`), directory index, MIME map |
| `src/file_cache.c`          | Bounded, ref-counted LRU cache of identity/gzip representations |
| `src/log.c`                 | Leveled JSON access/error logging; nonblocking pipe + writer thread; `SIGHUP` reopen |
| `src/sd_notify.c`           | Dependency-free `sd_notify` (`READY`/`STOPPING`) over `$NOTIFY_SOCKET` |
| `src/ring_buffer.c`         | Bounded circular byte buffer and line reader with rollback           |
| `src/metrics.c`             | Lock-free counters, JSON snapshot rendering, reporter thread         |
| `src/memory_profiler.c`     | `/proc` + `mallinfo2()` sampling (reporter thread only)              |
| `include/config.h`          | Runtime-tunable `ServerConfig` and its log-level enum                |
| `include/server_config.h`   | Every compile-time limit and default, and the runtime `ENV_*` names   |

## Configuration

- `config_defaults()` seeds `ServerConfig` from `server_config.h`; then the
  config file (`HTTP_SERVER_CONFIG` / `--config`, or the baked-in
  `DEFAULT_CONFIG_FILE` when neither is set), environment, and CLI are applied
  in that order, each overriding the previous.
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
  file and, when TLS is enabled, sets a flag that event-loop 0 consumes on its
  deadline tick to reload the certificate in place. `sd_notify(STOPPING)` is
  emitted during shutdown.


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
    `tls_init()` (cert/key load; no-op when disabled),
    `initialize_static_responses()`, then bind/listen, `sd_notify(READY)`, and
    enter the loops.

## Connection lifecycle

`ELConnState` (`include/event_loop.h:12`):

```
CONN_TLS_HANDSHAKE --SSL_accept done--> CONN_READING_HEADERS
   (TLS sockets only)                        |
                                             | complete request
                                             v
                                  (enqueue response) --+--> CONN_WRITING
                                                             |
                     CONN_READING_HEADERS <-- drained <------+
                          (idle deadline)         (keep-alive)
```

- `process_input()` (`src/event_loop.c:371`) pulls complete requests from the
  loop-owned `RingBuffer` and calls `el_prepare_response()` for each, up to
  `MAX_PIPELINE_DEPTH` per read pass. Responses are queued in the connection's
  `PendingResponse pq[]` ring.
- `el_prepare_response()` (`src/http_server.c`) is **transactional**: on an
  incomplete request it restores the ring buffer's `tail`/`size` and returns 1
  so the loop buffers more bytes. On success it fills a descriptor whose
  `body` references **cached memory** and whose header block is either a
  generated `header_buf` (`header = NULL`) or a static literal.
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

## Static assets and response semantics (Phase 1)

- At startup `initialize_static_responses()` reads `home.html` and `hello.html`
  **by relative path** and precompresses both with zlib. This is why the server
  must run from the repository root, and why a missing asset makes startup fail.
  Each file's `st_mtime` and size build a strong `ETag` and `Last-Modified`; the
  gzip representation gets a distinct ETag.
- `el_prepare_response()` parses the request, then builds the response header
  block **dynamically** into `PendingResponse.header_buf` with `header = NULL`
  (the event loop sends `header_buf` when `header` is null). This is what makes
  `Date`, validators, ranges, and negated encodings possible; the pre-Phase-1
  code reused two fixed header blocks and omitted `Date`.
- Parsing is strict (RFC 9110/9112): the request line may use runs of SP/HTAB,
  only `HTTP/1.0`/`HTTP/1.1` are accepted (`505` otherwise), HTTP/1.1 requires a
  `Host` header, and absolute-form targets are accepted and reduced to their
  path. Control characters, obs-fold, duplicate/conflicting `Content-Length`,
  and `Transfer-Encoding` are rejected (`400`/`501`); request-line and header
  limits yield `414`/`431`.
- Methods: `GET`/`HEAD` serve the resource; `OPTIONS` returns `204` with
  `Allow`; `POST` returns `405` with `Allow`; unknown methods return `501`. A
  request body is never read, so any body-bearing or body-implying request is
  answered and closed rather than risking connection desynchronization.
- Conditional requests: `If-None-Match` (weak comparison, `*`), then
  `If-Modified-Since`, produce `304`; `If-Range` gates a range on a matching
  validator.
- Ranges are served from the **identity** representation (so bytes are
  decodable and multipart parts stay coherent); content negotiation still picks
  the representation for full responses. A single satisfiable
  `Range: bytes=...` produces `206` with `Content-Range` and
  `Accept-Ranges: bytes`; when every range-spec is valid but unsatisfiable the
  response is `416`. Two or more satisfiable ranges produce a
  `206 multipart/byteranges` body (`format_multipart()`); the body is assembled
  on the heap and `PendingResponse.owned_body` tracks it for the event loop to
  free. The set is bounded by `MAX_MULTIPART_RANGES` and `MAX_MULTIPART_BYTES`;
  a range-set that exceeds either, or is syntactically invalid, is ignored and
  answered with the full negotiated `200` (RFC 9110 permits ignoring `Range`).
- Content negotiation: `Accept-Encoding` selects the gzip representation when
  `gzip` (or `*`) has `q > 0`, identity otherwise; `Vary: Accept-Encoding` is
  always emitted on negotiable responses, and `406` when neither is acceptable.
- Routes (`src/http_server.c`): `/home` and `/hello` are fixed legacy aliases
  backed by the startup cache; every other path is resolved against
  `document_root` (see below). `/` serves the root directory's index file, or
  falls back to `home.html` when no index exists.

## Document-root serving (Phase 2)

- `initialize_static_responses(cfg)` configures `document_root`, `index_files`,
  the optional `mime_types` file, the hidden-file/symlink policies, and a
  bounded representation cache. The document root is opened once
  (`path_resolver_init`) and every request path is resolved relative to that
  descriptor.
- `path_resolver_open()` (`src/path_resolver.c`) percent-decodes the request
  path **exactly once**, rejecting NUL, backslash, DEL, and control bytes
  (`400`) and over-long results (`414`); normalizes `.` and `..`, refusing a
  `..` that would climb above the root and any hidden (leading-dot) segment
  unless allowed (`403`); then opens the result with
  `openat2(RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS [| RESOLVE_NO_SYMLINKS])`,
  falling back to an `O_NOFOLLOW` dirfd walk on kernels without `openat2`. A
  directory resolves to the first existing `index_files` entry (`404` if none;
  there is no directory listing). Anything that is not a regular file is
  refused.
- `mime_type_for_path()` maps the file extension through a builtin table plus
  any operator `mime.types` entries; the default is
  `application/octet-stream`.
- Response bodies have three forms. Memory-backed cached representations point
  `body` into a pinned `FileCache` entry (`cache_entry`); multipart bodies are
  heap-owned (`owned_body`); and files larger than `CACHE_MAX_FILE_BYTES` are
  streamed from `body_fd` at `body_file_off` with nonblocking `sendfile()`.
  The event loop releases all three forms in `pending_release()`
  (`pq_pop()`/`pq_release_all()`).
- The cache (`src/file_cache.c`) is an LRU with a byte budget and entry cap.
  `file_cache_insert()` takes ownership of freshly read bodies; a cache hit
  returns an entry with an extra reference that the response holds until the
  event loop releases it. Eviction skips referenced entries, so an in-flight
  response can never have its bytes freed underneath it; if the budget cannot
  be made to fit, the file is streamed instead of cached.
- Files above the cache threshold are served identity-only (no gzip
  representation) and still honor `If-None-Match`/`If-Modified-Since`,
  `If-Range`, single and multipart ranges (parts are `pread()` from the
  descriptor).

## TLS termination (Phase 3)

- `tls_init()` builds one process-wide `SSL_CTX` (`src/tls.c`) shared by every
  loop: minimum TLS 1.2, TLS 1.3 preferred, compression and renegotiation
  disabled, an ECDHE-only cipher list, ALPN advertising `http/1.1`, and session
  resumption enabled. 0-RTT/early data is left disabled. `tls_new_conn()` creates
  a per-connection server `SSL` with `SSL_MODE_ENABLE_PARTIAL_WRITE` and
  `SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER`.
- An accepted TLS socket enters `CONN_TLS_HANDSHAKE`. The event loop drives
  `SSL_accept()` through `tls_handshake_step()`, re-arming `EPOLLIN`/`EPOLLOUT`
  on `SSL_ERROR_WANT_READ`/`WANT_WRITE`; the `header_read_timeout` deadline
  bounds the handshake. On completion the loop reads any already-buffered
  request and continues through the normal parser/queue path.
- TLS `SSL_read`/`SSL_write` replace `recv`/`send` on TLS connections. Because
  TLS cannot use `sendfile(2)`, a file-backed body is streamed with a bounded
  per-connection `pread`+`SSL_write` buffer (`TLS_FILE_BUF_SIZE`); memory and
  multipart bodies go straight through `SSL_write`. `SSL_free()` (after a
  best-effort `SSL_shutdown()`) runs in `conn_close()`.
- Overload of the TLS listener closes the socket promptly rather than sending a
  plaintext `503` that cannot be framed before a handshake.
- TLS counters (`tls_connections`, `tls_handshakes`, `tls_resumptions`,
  `tls_handshake_failures`) are emitted in the metrics snapshot. `SIGHUP` sets a
  reload flag; the deadline tick of loop 0 calls `tls_reload_if_requested()` to
  reload the certificate in place. Existing `SSL` objects keep the certificate
  they started with, so no connection is dropped.

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
   event loop must never `free()` them. Responses own two other resources: a
   multipart body (`owned_body`, aliased by `body`) and a streamed file
   (`body_fd`/`body_file_off`), plus a pinned cache entry (`cache_entry`). The
   event loop releases all of them through `pending_release()` in `pq_pop()`
   after sending or in `pq_release_all()` when a connection closes with
   responses still queued. When `header` is NULL the in-struct `header_buf` is
   sent instead; never point `header` at a response's own `header_buf`, because
   the pipeline queue copies the struct by value and the pointer would then
   reference the source copy. `body_fd` must be `-1` (not 0) when unused, or the
   event loop would close stdin.
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
9. Document-root paths are decoded once and opened only through
   `path_resolver_open()`. Never build a filesystem path from a request and
   `open()` it directly: the resolver is the single audited boundary that keeps
   every open beneath the root.
10. TLS uses one shared `SSL_CTX` and a per-connection `SSL` owned by exactly
    one loop; a connection's `SSL` and its streaming buffer are freed in
    `conn_close()`. Certificate/key material is loaded only through
    `src/tls.c`, and a world-accessible private key is fatal. Reload mutates the
    shared context's certificate only; already-established sessions and
    handshakes keep the certificate they started with.

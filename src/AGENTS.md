# src/ Notes

Implementation of the server. Read `../docs/architecture.md` before changing the
event loop. Directory-wide rules (build, tests, plans) are in `../AGENTS.md`.

## Files

- `main.c` — startup only. Order matters: config load → allocator cap →
  logging → metrics reporter → signal handlers → config print →
  privilege validation → loop-count resolution → FD preflight → effective
  capacity → host report → affinity → static assets → bind/listen →
  `metrics_set_ready(1)` + `sd_notify(READY)` → `event_loop_run` (create all
  listeners → privilege drop → start loops). The `SIGINT`/`SIGTERM` handler
  clears readiness before the drain; the `SIGHUP` handler requests log reopen,
  TLS reload, and safe-config reload. Keep
  `EL_THREAD_COUNT` compile-time; do not read it from the environment.
- `privilege.c` — resolves `run_user`/`run_group` (`privilege_validate`, called
  from `main.c` before binding) and drops to the identity
  (`privilege_drop`, called from `event_loop_run` after every listener exists and
  before any loop thread). Sets all three uids/gids, clears groups, and applies
  `no_new_privs`/non-dumpable. The drop is irreversible; failures are fatal.
- `config.c` — the single validated configuration surface (defaults < file <
  env < CLI). One `CFG_KEYS` table drives lookup, validation, and usage.
  `config_request_reload()`/`config_reload_if_requested()` re-parse the recorded
  sources on `SIGHUP` so the caller can apply the live-safe subset (log level).
- `log.c` — leveled JSON logging: format on the hot path, write to a
  nonblocking pipe, drain on a writer thread; `SIGHUP` reopens the file. The
  maximum level is an atomic so `SIGHUP` can change it live; with `syslog`
  enabled the writer thread also mirrors complete records to `syslog(3)`. Never
  block a producer; drop and count instead.
- `sd_notify.c` — `READY`/`STOPPING` datagrams to `$NOTIFY_SOCKET`; no
  libsystemd dependency.
- `tls.c` — OpenSSL context/policy (TLS 1.2+ min, ECDHE ciphers, ALPN
  `http/1.1`), cert/key load with permission checks, and the `SIGHUP` reload
  flag/consumer. The event loop owns per-connection `SSL` objects and the TLS
  record I/O; this file never touches a connection.
- `event_loop.c` — the nonblocking `epoll` state machine. One instance per
  thread; a connection is owned by exactly one loop. Response pointers are
  borrowed from cached/static memory and must never be freed here; the owned
  resources (`owned_body`, `body_fd`, `cache_entry`) are released by
  `pending_release()` in `pq_pop()`/`pq_release_all()`. Each
  listener pass handles at most `EL_ACCEPT_BATCH_SIZE` accepted sockets; at
  capacity it attempts a nonblocking 503 and closes promptly rather than
  disabling listener interest. On shutdown it stops accepting and drains within
  `config->shutdown_drain_timeout_sec`. It also drives the optional TLS
  listener: a `CONN_TLS_HANDSHAKE` state around `SSL_accept()`, TLS
  `SSL_read`/`SSL_write` wrappers, and a bounded `pread`+`SSL_write` path for
  file bodies (TLS cannot `sendfile`). Both listeners are set nonblocking.
- `http_server.c` — socket creation (plaintext and TLS listeners via one
  helper), strict HTTP/1.1 parsing, conditional
  requests, byte ranges, content negotiation, and static/gzip caching. Response
  headers are generated into `PendingResponse.header_buf` (with `header = NULL`)
  rather than precomputed; `Date`, validators, and `Vary` are added there.
  `el_prepare_response()` is transactional: roll the ring buffer back to its
  saved `tail`/`size` on incomplete input, or pipelining corrupts. Multiple
  satisfiable ranges produce a bounded `206 multipart/byteranges` whose body is
  heap-owned via `PendingResponse.owned_body` (freed by the event loop); an
  invalid or over-bounds range-set is ignored (`200`). Request bodies are never
  read. Phase 2 adds document-root routing: the `/home` and `/hello` aliases are
  served from the startup cache; every other path goes through
  `path_resolver_open()` and is served from the bounded cache (memory) or the
  file descriptor (`body_fd`, streamed with `sendfile`). Phase 5 adds the
  configurable `/metrics`/health/readiness endpoints (matched before the
  document root while `observability` is on) and a per-request `request_id`
  emitted as `X-Request-Id` and in the access log.
- `path_resolver.c` — the single audited path-resolution boundary: decode once,
  normalize, enforce hidden/symlink policy, `openat2(RESOLVE_BENEATH)` with an
  `O_NOFOLLOW` fallback, directory-index lookup, and the MIME map. Never open a
  request-derived path anywhere else.
- `file_cache.c` — bounded, ref-counted LRU cache of identity/gzip
  representations. `file_cache_insert()` takes ownership of freshly read bodies;
  eviction skips referenced entries so a borrowed in-flight body is never freed.
- `ring_buffer.c` — bounded circular buffer and line reader.
- `metrics.c` — lock-free counters, the request-latency histogram, JSON snapshot
  and Prometheus rendering, and the readiness gauge. `memory_profiler_get()`
  exposes the last cached memory sample to the Prometheus renderer.
- `memory_profiler.c` — procfs/`mallinfo2` sampling, reporter thread only.

## Editing rules

- Adding or removing a `.c` file here requires `cmake -S . -B .` (the build uses
  `file(GLOB ...)`).
- New compile-time limits belong in `../include/server_config.h`; new runtime
  keys additionally need a `CFG_KEYS` row in `config.c` and a `config_defaults`
  entry. Never a literal.
- Match the existing C style: C11, `-Wall -Wextra -pedantic`, no new warnings.

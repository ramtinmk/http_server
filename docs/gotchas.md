# Gotchas

Sharp edges that cost time. Each entry says what bites and how to avoid it.

## Build

- **Run from the repository root.** `root/home.html` and `root/hello.html` are
  opened by relative path at startup; a server launched elsewhere exits during
  static-asset preflight.
- **Executables land in `bin/`, not `build/`.** `CMAKE_RUNTIME_OUTPUT_DIRECTORY`
  is set to `bin/` in `CMakeLists.txt:17`. Ignore stale root-level binaries
  (e.g. `./test`).
- **`file(GLOB ...)` does not notice new/removed files.** After adding or
  deleting any `src/*.c` or `tests/*.c`, run `cmake -S . -B .` before `make`,
  otherwise the new file is silently not compiled. This is the most common
  "my code changes have no effect" cause.
- **Generated CMake files are git-ignored on purpose** (`CMakeCache.txt`,
  `Makefile`, `compile_commands.json`, `Testing/`) because they embed absolute
  paths. Never commit them.
- **`make lint` may not exist.** The `lint`/`lint-fix` targets are only defined
  when `clang-tidy` is installed; otherwise CMake prints a warning. `lint-fix`
  edits source files in place.

## Runtime and configuration

- **`EL_THREAD_COUNT` is a compile-time macro, not an environment variable.**
  `EL_THREAD_COUNT=4 ./bin/http_server` has zero effect. Set it with
  `-DCMAKE_C_FLAGS="-DEL_THREAD_COUNT=4"` and rebuild, or edit
  `include/server_config.h`. The same applies to every limit in that header. See
  `docs/env-vars.md`.
- **`HTTP_SERVER_ACCESS_LOG` is now live.** Setting it to `0` disables access
  records (the benchmark harnesses rely on this); `1` emits one JSON line per
  completed response. Error/info records follow `HTTP_SERVER_LOG_LEVEL`.
- **Access logging never blocks a loop, but it can drop.** Records go through an
  `O_NONBLOCK` pipe to a writer thread; when the pipe is full the record is
  dropped and counted (`dropped_logs` in the shutdown record) instead of
  stalling the event loop.
- **`SIGHUP` reopens the log file, reloads the TLS certificate, and re-reads the
  log level; it does not reload connection limits or timeouts.** Log reopening
  is polled by the writer thread at `LOG_POLL_INTERVAL_MS` (200 ms); the cert and
  log-level reloads are consumed by event-loop 0 on its `EL_DEADLINE_SCAN_MS`
  tick. Timeouts, capacity, and `max_keepalive_requests` are read once at startup
  (existing connections already hold their deadlines), so changing them needs a
  restart. See `docs/runbooks/observability-and-reload.md`.
- **Unknown config *file/CLI* keys are fatal; unknown environment variables are
  ignored.** Only the named `HTTP_SERVER_*` variables are read, so unrelated
  variables in the environment are harmless.
- **Startup refuses to run on a low FD limit.** The check is
  `max_connections + REQUIRED_NOFILE_HEADROOM + REQUIRED_NOFILE_PER_LOOP × loops`
  using the runtime `max_connections` value (default `MAX_ACTIVE_CONNECTIONS`).
  On a many-core host the per-loop term is large; either `ulimit -n` higher,
  lower `HTTP_SERVER_MAX_CONNECTIONS`, or pin to fewer cores. The failure
  message names each term.
- **Capacity is a `min` of three things**, so raising
  `HTTP_SERVER_MAX_CONNECTIONS` alone may not raise the effective capacity. The
  startup line prints `operator_max`, `descriptor_cap`, `effective`, and what
  limited it.
- **Over-capacity connections are accepted and rejected in bounded batches.**
  The server attempts a complete nonblocking 503, then resets promptly if the
  socket cannot accept the whole response. `EL_ACCEPT_BATCH_SIZE` limits work
  per listener dispatch; when the process is unable to accept at all (for
  example, descriptor exhaustion), normal kernel backlog timeout/refusal
  behavior can still occur.
- **A single-loop build intentionally fails to share the port.** `SO_REUSEPORT`
  is only set when multiple loops are enabled, so a second accidental instance
  fails to bind instead of silently splitting traffic.
- **`HTTP_SERVER_CPU_SET` invalid ranges are fatal**, not warnings. Unset it if
  unsure.
- **Memory fields are Linux/best-effort.** Without `/proc/self/smaps_rollup` PSS
  is `0`; without glibc, heap counters are `0`. Check `memory_sample_ok` before
  trusting zeroes. Sampling only happens when `HTTP_SERVER_METRICS_FILE` is set.

## HTTP semantics (Phase 1)

- **Multi-range requests may be answered with the full body.** Two or more
  satisfiable ranges are assembled into a bounded `206 multipart/byteranges`
  response; if the set exceeds `MAX_MULTIPART_RANGES`/`MAX_MULTIPART_BYTES`, or
  is syntactically invalid, the `Range` header is ignored and the full `200` is
  returned (RFC 9110 permits this). When at least one range is satisfiable, the
  unsatisfiable ones are silently dropped; only an all-unsatisfiable set is
  `416`. Do not assume every multi-range request yields multipart.
- **Multipart bodies are the only heap-owned response bodies.** They are built
  in `format_multipart()` and referenced by `PendingResponse.owned_body`; the
  event loop frees them in `pq_pop()`/`pq_release_all()`. Every other body is
  borrowed from the startup cache and must never be freed. A body leaking or
  being double-freed here shows up under the disconnect-burst test
  (`test_el_capacity_and_fd_leak`).
- **The server never reads a request body.** `POST` returns `405` and any
  `GET`/`HEAD` carrying `Content-Length > 0` returns `400`; both close the
  connection. This avoids the connection desynchronization that occurs when
  unread body bytes are mistaken for the next pipelined request.
- **`Transfer-Encoding` is always rejected** (`501`, or `400` when combined
  with `Content-Length`). Chunked request framing is not supported.
- **The ETag differs per representation.** The gzip variant has a `-gzip` suffix
  so a client that conditions on the identity ETag is not served stale gzip.
- **`Date` is cached per event-loop thread and refreshed once per second**, so a
  response's `Date` can trail wall-clock by under a second. This is intentional
  and keeps `gmtime` off the hot path.
- **Ranges are served from the identity representation.** A request with both
  `Accept-Encoding: gzip` and a `Range` still gets an identity `206` (no
  `Content-Encoding`), because a partial gzip stream is not decodable and
  multipart parts would not be coherent. `ETag`/`Last-Modified` in a range
  response are the identity validator; `Vary: Accept-Encoding` is still sent.
  When the client sends `identity;q=0` the `Range` is ignored and the whole gzip
  `200` is returned.

## Static file serving (Phase 2)

- **The server still needs `root/home.html`/`root/hello.html` under the working
  directory.** They back the `/home`, `/hello`, and (fallback) `/` aliases; a
  missing asset fails startup before the document root matters.
- **Hidden means any leading-dot path segment**, not just `.git`. `/..../x`,
  `/.env`, and `/.well-known/...` are all `403` while `hidden_files=0`. Set
  `HTTP_SERVER_HIDDEN_FILES=1` to serve them.
- **`symlinks=1` only follows symlinks that stay beneath the root, and needs
  `openat2` (Linux 5.6+).** An absolute symlink (`link -> /etc/passwd`) is
  refused even when symlinks are allowed because `RESOLVE_BENEATH` cannot prove
  it stays in-root. On older kernels the fallback walk refuses all symlinks.
- **Cache budget of `0` disables caching**: every file streams from its
  descriptor (identity only), which is the simplest rollback if the cache is
  ever suspect.
- **Files larger than `CACHE_MAX_FILE_BYTES` (1 MiB) are never cached or
  gzipped.** They stream identity-only but still support conditional requests
  and ranges; `Accept-Encoding: gzip` yields an identity `200`, not a `406`.
- **A config file is always loaded.** When neither `--config` nor
  `HTTP_SERVER_CONFIG` is set the server loads the checked-in
  `http_server.conf` (baked in as `DEFAULT_CONFIG_FILE`, an absolute path). The
  file is fatal if missing, so a moved build tree needs a `cmake` re-run; set
  `-DDEFAULT_CONFIG_FILE=` to build a server with no default file. This also
  means `./bin/http_server` now resolves `document_root` from that file rather
  than from the compiled `.` default.
- **The shipped document root is `root`** (set by `http_server.conf`); the
  compiled `DOCUMENT_ROOT` fallback is still `.`. A relative `document_root` is
  resolved against the process working directory, so the "run from the repo
  root" rule still applies unless an absolute root is configured. The `/home`
  and `/hello` aliases are loaded from `root/home.html`/`root/hello.html` (a
  fixed path relative to the working directory), **not** from the configured
  `document_root`, so those files must stay under the repo's `root/` directory
  for the aliases to work.
- **Path decoding happens exactly once.** A double-encoded `%252e%252e` is
  treated as the literal filename `%2e%2e`, not as `..`; NUL (`%00`),
  backslashes, and other control bytes are `400` rather than being normalized.

## TLS termination (Phase 3)

- **TLS is off by default and needs the OpenSSL dev package to build.**
  `find_package(OpenSSL REQUIRED)` makes configure fail without `libssl-dev`.
  Enable at runtime with `HTTP_SERVER_TLS=1` plus `tls_cert_file`/`tls_key_file`;
  the plaintext listener is unaffected and remains the fallback.
- **`tls_port` must differ from `port`.** The two listeners are independent; a
  shared port would fail one bind (single loop) or split traffic (multi-loop).
- **A world-accessible private key is fatal.** The startup check refuses a key
  with group/other write or other-read bits (`chmod 600`); a group-readable key
  only warns. OpenSSL parse errors and a cert/key mismatch are also fatal and
  name the path.
- **The TLS listener must be nonblocking.** Both listeners are set nonblocking
  in `loop_init`; a blocking TLS listener makes `accept4` stall the event loop
  and the handshake never runs. If TLS connections hang while plaintext works,
  check this first.
- **`sendfile` is not available over TLS**, so file-backed bodies use a bounded
  `pread`+`SSL_write` buffer (`TLS_FILE_BUF_SIZE` per in-flight streaming TLS
  response). Cached and multipart bodies are written directly.
- **ALPN server lists are length-prefixed wire format.** The select callback
  passes `"\x08http/1.1"` (a length byte, then the name); passing a bare
  `"http/1.1"` makes `SSL_select_next_proto` read `'h'` as the length and never
  negotiate.
- **A `WANT_WRITE` from `SSL_read` is retried on the next readable event**, not
  by arming `EPOLLOUT`, to avoid a level-triggered busy loop. TLS 1.2
  renegotiation is disabled, so this path is essentially unreachable in
  practice.
- **`SIGHUP` reload keeps connections.** The certificate is reloaded into the
  live context; established sessions keep their original certificate and a
  failed reload keeps the previous one.

## Hardening (Phase 4)

- **The privilege drop happens after binding, not before.** All listeners are
  created first (so a low port / `CAP_NET_BIND_SERVICE` still works), then
  `privilege_drop()` runs before any loop thread. A client can complete a TCP
  connect while the process is still privileged; the connection is not serviced
  until after the drop.
- **`run_user` is irreversible.** All three uids/gids are overwritten. The
  document root, log file, and TLS certificate are opened before the drop, but
  per-request file opens and the `SIGHUP` cert reload run as `run_user`; a
  root-only cert makes the reload fail (the previous cert stays active).
- **A configured identity requires starting as root** unless it equals the
  current uid/gid. Running unprivileged with `run_user` set to another account
  is fatal, naming the key.
- **`_FORTIFY_SOURCE=2` only applies to optimized builds.** It is a no-op (and
  would warn) at `-O0`, so the flag is scoped to non-Debug configurations.
- **Hardening flags are compile-probed.** `-DENABLE_HARDENING=OFF` reverts to
  the previous unhardened build; `readelf` on `bin/http_server` should show
  `Type: DYN`, `GNU_RELRO`, `BIND_NOW`, and a non-executable `GNU_STACK`.

## Observability (Phase 5)

- **The observability endpoints are off by default and share the data
  listeners.** `/metrics`, `/healthz`, and `/readyz` are only served when
  `observability=1`, and they are matched before document-root resolution on the
  plaintext and TLS ports. There is no authentication or separate admin port, so
  firewall the endpoints if the port is public, and keep the configured paths
  clear of real assets (they shadow the file at that path).
- **`/readyz` returns `503` only in the small window between the signal and the
  loops removing their listeners.** Once the drain starts the listeners are
  removed, so a probe gets connection-refused instead of `503`. Both mean "not
  ready"; treat refusal as a failed probe.
- **The Prometheus body is capped at `METRICS_PROM_MAX`** and truncated rather
  than grown. The JSON snapshot written for `HTTP_SERVER_METRICS_FILE` is a
  separate format and is unchanged.
- **Request IDs are per process, not global.** The seed is `wall-clock ^ pid`, so
  two concurrent servers can in principle collide; the id is for correlating a
  response with a log line, not for security.
- **`simplehttp_resident_memory_bytes`/`simplehttp_virtual_memory_bytes` reflect
  the last memory-profiler sample and are `0` unless `HTTP_SERVER_METRICS_FILE`
  started the reporter thread.** The `/metrics` handler never samples procfs
  itself: the reporter thread is the sole sampler (see
  `src/memory_profiler.c`).
- **`syslog=1` mirrors records from the writer thread**, not the event loops, so
  a syslog target cannot stall a loop. A full pipe still drops records (counted
  in `dropped_logs`).

## Tests

- **The server suite needs a running server** on `127.0.0.1:8081`:
  `./bin/run_tests server`. Bare `./bin/run_tests` runs every suite and has the
  same prerequisite.
- **`ctest` is not fully server-free.** `unit_tests` runs `run_tests` (all
  suites); start the server first when invoking the full CTest suite. Only
  `benchmark_python_tests` is independent.
- **New test files are not auto-wired.** `tests/*.c` are globbed into
  `run_tests`, but `tests/main_test.c` must explicitly call a suite's
  `run_*_tests()` function. Adding a file alone runs nothing.
- **Test policy is strict** (`AGENTS.md`): prefer E2E, never write unit tests
  after the code, no tautological or change-detector tests.

## Benchmarking

- **Artifacts are committed.** Harnesses append to `benchmarks/*.csv` / `*.json`;
  that is intended evidence, not scratch output.
- **`--start-server` harnesses assume the repo root** and spawn
  `./bin/http_server`. Run them from the root.
- **Above ~1.4M req/s on loopback, `wrk` is the bottleneck**, not the server.
  Treat the top of the pipelining grid in `readme.md` as a client-side bound.
- **Reproducible runs need pinning and the performance governor.** Use
  `scripts/run_benchmark_pinned.sh` and `--require-governor`; otherwise core
  migration and frequency scaling dominate run-to-run variance.

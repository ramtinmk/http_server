# Gotchas

Sharp edges that cost time. Each entry says what bites and how to avoid it.

## Build

- **Run from the repository root.** `home.html` and `hello.html` are opened by
  relative path at startup; a server launched elsewhere exits during static-asset
  preflight.
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
- **`SIGHUP` reopens the log file, it does not reload config.** Reopening is
  polled by the writer thread at `LOG_POLL_INTERVAL_MS` (200 ms). Config and
  runtime limits are read once at startup.
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

# Repository Instructions

C11 static-file HTTP/1.1 server (POSIX sockets, `epoll`, pthreads, zlib). This
file is the router: hard, repo-wide rules live here; depth lives under `docs/`
and in the nearest directory's `AGENTS.md`.

## Where to look

| Need                                             | Read                          |
|--------------------------------------------------|-------------------------------|
| Module responsibilities, data flow, invariants   | `docs/architecture.md`        |
| Every environment variable and compile-time limit| `docs/env-vars.md`            |
| Sharp edges that waste time                      | `docs/gotchas.md`             |
| Task recipes (add a route, reproduce a benchmark)| `docs/runbooks/`              |
| File-class benchmark methodology and corpus      | `docs/benchmarks.md`          |
| Phase specs and intent                           | `plans/` (read `plan-spec.md` first) |

## Git branch selection

- At the start of a coding task, inspect the current branch and working tree. Follow an explicit branch name or instruction in the prompt; otherwise use the criteria below.
- Create a topic branch before editing for a standalone feature, bug fix, substantial refactor, or experiment when the current branch is the shared/default branch. Use a short task-based name such as `feature/<name>`, `fix/<name>`, `refactor/<name>`, or `experiment/<name>`.
- Stay on the current branch for small, localized changes, documentation-only work, follow-ups to work already on a suitable topic branch, or when the prompt clearly asks to continue the current branch.
- Never switch branches with unrelated uncommitted changes present, and never stash, discard, commit, or push changes unless asked. If a new branch seems warranted but the worktree is dirty, preserve it and ask before switching; otherwise continue on the current branch when safe.
- Do not create branches for informational questions. Do not commit or push as an automatic consequence of creating a branch.

## Update the docs in every iteration

Documentation is part of the change, not a follow-up task. **Every iteration
that changes behavior, limits, APIs, or workflows must update the relevant docs
in the same commit.** Do not leave doc updates for later.

- New/changed limit or environment variable → `docs/env-vars.md`.
- New/changed module responsibility, data flow, or invariant → `docs/architecture.md` and the nearest `<dir>/AGENTS.md`.
- New sharp edge or surprising failure mode → `docs/gotchas.md`.
- New repeatable task → add or edit a file under `docs/runbooks/`.
- Phase or plan work → tick the phase's checklist in `plans/` (see `## Plans`).
- Behavior visible to users → `readme.md`.

If a fact appears in more than one place, update every copy in the same
iteration. Keep this file a router (short, links out); put depth in `docs/`.

## Build

- Run commands from the repository root. The project uses C11, POSIX threads, zlib, and (Phase 3) OpenSSL (`libssl-dev`); CMake requires `OpenSSL`.
- The build defaults to `CMAKE_BUILD_TYPE=Release` (`-O2 -DNDEBUG`); pass `-DCMAKE_BUILD_TYPE=Debug` for an unoptimized build with symbols. Do not benchmark a Debug build.
- Build with `make`; CMake writes executables directly to `bin/`, not `build/` (`bin/http_server` and `bin/run_tests`). Ignore legacy root-level binaries.
- The binary is built hardened by default (PIE, full RELRO, NX stack, stack protector, `_FORTIFY_SOURCE=2`) behind `ENABLE_HARDENING`; disable with `-DENABLE_HARDENING=OFF`.
- `CMakeLists.txt` uses `file(GLOB ...)` for `src/*.c` and `tests/*.c`; after adding or removing a C file, regenerate with `cmake -S . -B .` before building.
- Targets include `make benchmark`, `make benchmark-tls`, `make corpus`, `make stress`, `make benchmark-matrix`, `make saturation`, `make phase0-lifecycle`, `make phase0-2x`, `make phase0-accesslog`, `make phase2-static`, `make phase3-tls`, `make phase4-hardening`, `make memory-soak`, `make startup-failfast`, `make lint`, and `make lint-fix`. `lint`/`lint-fix` exist only when clang-tidy is installed; `lint-fix` edits source files.

## Tests

- Focused suites do not need a running server: `./bin/run_tests ring`.
- The TLS suite is self-contained — it generates a throwaway self-signed certificate, forks its own server with plaintext + TLS listeners on ephemeral ports, and drives the TLS record path with an OpenSSL client: `./bin/run_tests tls`.
- The server suite is selected with `./bin/run_tests server` and requires `./bin/http_server` running on `127.0.0.1:8081`. Bare `./bin/run_tests` runs all suites (the `server` suite keeps that prerequisite; `ring` and `tls` do not).
- Run the server integration suite from the repository root:
  ```bash
  ./bin/http_server &
  SERVER_PID=$!
  sleep 0.5
  ./bin/run_tests server
  kill "$SERVER_PID" && wait "$SERVER_PID" 2>/dev/null || true
  ```
- `ctest --output-on-failure` runs the same server-dependent C runner plus the Python benchmark tests; start the server first when running the full CTest suite.

### Test policy

- NEVER write unit tests after you write code.
- Highly prefer E2E tests as the sole testing mechanism. Use them to verify complex features work. At the end of E2E tests, produce a verifiable and repeatable artifact.
- If you must test a system in isolation, FIRST write all the ways it could fail, THEN write the code.
- Tautological tests considered harmful.
- Change-detector tests considered harmful.
- Do not create regression tests for bug fixes without a genuine gap in behavior testing.

## Runtime

- `./bin/http_server` must be launched from the repository root because the startup-cached static files (`root/home.html` and `root/hello.html`) are opened via relative paths. The default port is `8081`. Runtime limits come from one validated surface — a config file (`HTTP_SERVER_CONFIG`/`--config`, defaulting to the checked-in `http_server.conf` via the compile-time `DEFAULT_CONFIG_FILE`), environment variables, and CLI flags (`--help` lists them) — resolved defaults < file < env < CLI. Invalid keys/values are fatal and name the key. `SIGTERM`/`SIGINT` drains in-flight responses within `HTTP_SERVER_SHUTDOWN_DRAIN_TIMEOUT`; `SIGHUP` reopens the log file and, when TLS is enabled, reloads the certificate.
- The server runs nonblocking `epoll` event loops, one per online CPU core by default; each loop binds a `SO_REUSEPORT` listener. `EL_THREAD_COUNT` is a **compile-time macro**, not an environment variable — passing `EL_THREAD_COUNT=4` on the command line has no effect. See `docs/env-vars.md`.
- Startup preflights the host and refuses to start when the effective soft `RLIMIT_NOFILE` is below `MAX_ACTIVE_CONNECTIONS + REQUIRED_NOFILE_HEADROOM + REQUIRED_NOFILE_PER_LOOP × event-loop-count`. `HTTP_SERVER_CPU_SET` (taskset list, e.g. `0-3`) pins the server; invalid ranges are fatal.
- TLS load is driven by `scripts/http_benchmark.py --tls` (client wraps its sockets in TLS; `--start-server` generates a throwaway certificate and puts the TLS listener on `--port`). `make benchmark-tls` runs the keep-alive scenario over TLS and records `hardware_agnostic_rps` in `benchmarks/tls_benchmark.csv`.
- Benchmark runs can use `HTTP_SERVER_METRICS_FILE=<path>` for server metrics; benchmark output is appended under `benchmarks/`. `python3 scripts/http_benchmark.py --check-env` verifies ulimit, `net.core.somaxconn`, core count, and (with `--require-governor`) the CPU governor; it exits non-zero naming the unmet requirement. `scripts/run_benchmark_pinned.sh` pins server/generator to disjoint CPU sets before running the matrix. `scripts/run_nginx_comparison.sh` builds and runs an nginx peer comparison over the file-class corpus (identity, gzip_static, TLS); see `docs/runbooks/compare-against-nginx.md`. See `docs/gotchas.md` for pitfalls (including the nonblocking, drop-on-full `HTTP_SERVER_ACCESS_LOG` path).

## Plans

- Before editing any file under `plans/`, read the root `plan-spec.md`; it defines required metadata, categories, phase structure, and acceptance criteria. Project-specific facts belong in this file or `docs/`, not in `plan-spec.md`.
- The changes made to the system should be incrementally check marked in the plan's each phase todolist.

## Code Map

- `src/main.c` owns socket setup, signal handling, startup preflight, accept/admission control, and dispatch selection. `src/privilege.c` owns the `run_user`/`run_group` drop (after binding) and the always-on `no_new_privs`/non-dumpable hardening. Directory notes: `src/AGENTS.md`.
- `src/event_loop.c` owns the default nonblocking `epoll` connection state machine; `src/http_server.c` owns HTTP parsing, static responses, keep-alive, and gzip handling.
- `src/tls.c` (with `include/tls.h`) owns the OpenSSL context, TLS 1.2+/ALPN policy, cert/key loading, and `SIGHUP` reload; `src/event_loop.c` drives the per-connection TLS state machine.
- `src/ring_buffer.c` provides buffering; `src/metrics.c` and `src/memory_profiler.c` provide runtime instrumentation.
- `include/server_config.h` is the single source of truth for every limit and its default.

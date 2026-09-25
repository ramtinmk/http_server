# Repository Instructions

## Build

- Run commands from the repository root. The project uses C11, POSIX threads, and zlib.
- Build with `make`; CMake writes executables directly to `bin/`, not `build/` (`bin/http_server` and `bin/run_tests`). Ignore legacy root-level binaries.
- `CMakeLists.txt` uses `file(GLOB ...)` for `src/*.c` and `tests/*.c`; after adding or removing a C file, regenerate with `cmake -S . -B .` before building.
- Available project targets include `make benchmark`, `make stress`, `make benchmark-matrix`, `make lint`, and `make lint-fix`. `lint-fix` edits source files; lint targets are only available when clang-tidy is installed.

## Tests

- Focused suites do not need a running server: `./bin/run_tests ring` and `./bin/run_tests thread_pool`.
- The server suite is selected with `./bin/run_tests server` and requires `./bin/http_server` running on `127.0.0.1:8081`. Bare `./bin/run_tests` runs all suites and therefore has the same prerequisite.
- Run the server integration suite from the repository root:
  ```bash
  ./bin/http_server &
  SERVER_PID=$!
  sleep 0.5
  ./bin/run_tests server
  kill "$SERVER_PID" && wait "$SERVER_PID" 2>/dev/null || true
  ```
- `ctest --output-on-failure` includes the same server-dependent C test runner plus the Python benchmark tests; start the server first when running the full CTest suite.


- NEVER write unit tests after you write code. 
- Highly prefer E2E tests as the sole testing mechanism. Use them to verify complex features work. At the end of E2E tests, produce a verifiable and repeatable artifact. 
- If you must test a system in isolation, FIRST write all the ways it could fail, THEN write the code.
- Tautological tests considered harmful.  
- Change-detector tests considered harmful.  
- Do not create regression tests for bug fixes without a genuine gap in behavior testing.


## Runtime

- `./bin/http_server` must be launched from the repository root because static files (`home.html` and `hello.html`) are opened via relative paths. The default port is `8081`.
- The default build runs nonblocking epoll event loops (`USE_EVENT_LOOP=1` in `include/server_config.h`), not the older blocking thread-pool dispatch path. It starts one loop per online CPU core by default (`EL_THREAD_COUNT=0` auto-detects; a positive value overrides, and `1` is the single-loop control) and each loop binds a `SO_REUSEPORT` listener.
- Benchmark runs can use `HTTP_SERVER_METRICS_FILE=<path>` for server metrics; benchmark output is appended under `benchmarks/`.
- Startup preflights the host: it prints/raises `RLIMIT_NOFILE` and refuses to start unless the effective soft limit is at least `MAX_ACTIVE_CONNECTIONS + REQUIRED_NOFILE_HEADROOM + REQUIRED_NOFILE_PER_LOOP × event-loop-count`. `HTTP_SERVER_CPU_SET` (taskset list, e.g. `0-3`) pins the server; invalid ranges are fatal.
- `python3 scripts/http_benchmark.py --check-env` verifies ulimit, `net.core.somaxconn`, core count, and (with `--require-governor`) the CPU governor; it exits non-zero naming the unmet requirement. `scripts/run_benchmark_pinned.sh` pins server/generator to disjoint CPU sets before running the matrix.

## Plans

- Before editing any file under `plans/`, read the root `plan-spec.md`; it defines required metadata, categories, phase structure, and acceptance criteria. Project-specific facts belong here, not in `plan-spec.md`.
- the changes made to the system should be incrementally check marked in the plan's each phase todolist

## Code Map

- `src/main.c` owns socket setup, signal handling, accept/admission control, and dispatch selection.
- `src/event_loop.c` owns the default nonblocking epoll connection state machine; `src/http_server.c` owns HTTP parsing, static responses, keep-alive, and gzip handling.
- `src/ring_buffer.c`, `src/thread_pool.c`, and `src/metrics.c` provide buffering, the legacy Phase 2 worker pool, and runtime metrics respectively.

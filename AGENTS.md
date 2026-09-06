# AGENTS.md

## Build & Output Locations

- **Build command**: `make` (or `cmake . && make`).
- **Binary outputs**: Built binaries are written directly to `./bin/` (`bin/http_server` and `bin/run_tests`), **not** `./build/`.
- **Stale root binaries**: Ignore any legacy `./http_server` or `./test` executables in the root directory; always run from `./bin/`.
- **Adding new files**: `CMakeLists.txt` uses `file(GLOB ...)` for `src/` and `tests/`. Re-run `cmake .` whenever adding or removing `.c` files.
- **Dependencies**: C11, POSIX threads (`pthreads`), and `zlib` (`-lz`).

## Running Tests

Test runner binary: `./bin/run_tests`

- **Standalone unit suites** (no running server needed):
  - Ring buffer suite: `./bin/run_tests ring`
  - Thread pool suite: `./bin/run_tests thread_pool`
- **Server integration suite** (`./bin/run_tests server` or bare `./bin/run_tests`):
  - **Prerequisite**: Requires `./bin/http_server` actively running on `127.0.0.1:8080`.
  - Test command sequence:
    ```bash
    ./bin/http_server &
    SERVER_PID=$!
    sleep 0.5
    ./bin/run_tests server
    kill $SERVER_PID && wait $SERVER_PID 2>/dev/null || true
    ```

## Runtime Quirks & Architecture

- **Working directory requirement**: Always run `./bin/http_server` from the repository root. Static routes look up `home.html` and `hello.html` via relative paths from CWD.
- **Port**: Default is `8080` (configured via `PORT` macro in `include/http_server.h`).
- **Signal handling**: `SIGPIPE` is ignored globally (`SIG_IGN`) and sends use `MSG_NOSIGNAL` to prevent crashes on abruptly disconnected clients.
- **Core modules**:
  - `src/main.c`: Server socket setup, accept loop, and task dispatch to thread pool.
  - `src/http_server.c`: HTTP/1.1 request line/header parsing, keep-alive, and chunked gzip response streaming via zlib.
  - `src/ring_buffer.c`: Dynamic ring buffer with line-parsing transaction rollback support (`ring_buffer_readline`).
  - `src/thread_pool.c`: Fixed worker pool (`THREAD_POOL_SIZE = 16`) with preallocated task arena.

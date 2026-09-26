# src/ Notes

Implementation of the server. Read `../docs/architecture.md` before changing the
event loop. Directory-wide rules (build, tests, plans) are in `../AGENTS.md`.

## Files

- `main.c` — startup only. Order matters: allocator cap → metrics reporter →
  signal handlers → config print → loop-count resolution → FD preflight →
  effective capacity → host report → affinity → static assets → bind/listen.
  Keep `EL_THREAD_COUNT` compile-time; do not read it from the environment.
- `event_loop.c` — the nonblocking `epoll` state machine. One instance per
  thread; a connection is owned by exactly one loop. Response pointers are
  borrowed from cached/static memory and must never be freed here.
- `http_server.c` — socket creation, HTTP parsing, and static/gzip caching.
  `el_prepare_response()` is transactional: roll the ring buffer back to its
  saved `tail`/`size` on incomplete input, or pipelining corrupts.
- `ring_buffer.c` — bounded circular buffer and line reader.
- `metrics.c` — lock-free counters and JSON snapshot rendering.
- `memory_profiler.c` — procfs/`mallinfo2` sampling, reporter thread only.

## Editing rules

- Adding or removing a `.c` file here requires `cmake -S . -B .` (the build uses
  `file(GLOB ...)`).
- New limits belong in `../include/server_config.h`, not as literals.
- Match the existing C style: C11, `-Wall -Wextra -pedantic`, no new warnings.

# src/ Notes

Implementation of the server. Read `../docs/architecture.md` before changing the
event loop. Directory-wide rules (build, tests, plans) are in `../AGENTS.md`.

## Files

- `main.c` — startup only. Order matters: config load → allocator cap →
  logging → metrics reporter → signal handlers → config print → loop-count
  resolution → FD preflight → effective capacity → host report → affinity →
  static assets → bind/listen → `sd_notify(READY)` → loops. Keep
  `EL_THREAD_COUNT` compile-time; do not read it from the environment.
- `config.c` — the single validated configuration surface (defaults < file <
  env < CLI). One `CFG_KEYS` table drives lookup, validation, and usage.
- `log.c` — leveled JSON logging: format on the hot path, write to a
  nonblocking pipe, drain on a writer thread; `SIGHUP` reopens the file. Never
  block a producer; drop and count instead.
- `sd_notify.c` — `READY`/`STOPPING` datagrams to `$NOTIFY_SOCKET`; no
  libsystemd dependency.
- `event_loop.c` — the nonblocking `epoll` state machine. One instance per
  thread; a connection is owned by exactly one loop. Response pointers are
  borrowed from cached/static memory and must never be freed here. Each
  listener pass handles at most `EL_ACCEPT_BATCH_SIZE` accepted sockets; at
  capacity it attempts a nonblocking 503 and closes promptly rather than
  disabling listener interest. On shutdown it stops accepting and drains within
  `config->shutdown_drain_timeout_sec`.
- `http_server.c` — socket creation, HTTP parsing, and static/gzip caching.
  `el_prepare_response()` is transactional: roll the ring buffer back to its
  saved `tail`/`size` on incomplete input, or pipelining corrupts.
- `ring_buffer.c` — bounded circular buffer and line reader.
- `metrics.c` — lock-free counters and JSON snapshot rendering.
- `memory_profiler.c` — procfs/`mallinfo2` sampling, reporter thread only.

## Editing rules

- Adding or removing a `.c` file here requires `cmake -S . -B .` (the build uses
  `file(GLOB ...)`).
- New compile-time limits belong in `../include/server_config.h`; new runtime
  keys additionally need a `CFG_KEYS` row in `config.c` and a `config_defaults`
  entry. Never a literal.
- Match the existing C style: C11, `-Wall -Wextra -pedantic`, no new warnings.

# include/ Notes

Public headers. Repo-wide rules: `../AGENTS.md`; data flow and invariants:
`../docs/architecture.md`.

- `server_config.h` is the **single source of truth for every limit and its
  default**, plus the runtime `ENV_*` names. Every definition is
  `#ifndef`-guarded so a `-D` compile flag overrides it. The comment above each
  macro is the rationale — update it when you change the value, and mirror the
  change in `../docs/env-vars.md`.
- `config.h` defines the runtime `ServerConfig` and `LogLevel`. Runtime keys
  also need a `CFG_KEYS` row in `src/config.c`.
- `log.h` is the leveled JSON logging surface. Hot-path calls must not block;
  the implementation drops records when its pipe is full.
- `sd_notify.h` is a dependency-free systemd readiness shim.
- `http_server.h` defines `PendingResponse` and `PR_HEADER_BUF_SIZE`. Its
  `header`/`body` pointers alias cached or static memory; the event loop must
  never `free()` them. The exception is `owned_body` (multipart/byteranges): it
  aliases `body` and the event loop frees it after send or on close. When
  `header` is NULL the in-struct `header_buf` is sent instead (never point
  `header` at a response's own `header_buf`; the queue copies the struct by
  value). The `method`/`path`/`started` fields are access-log metadata.
- `event_loop.h` owns the connection state enum, close reasons, and the
  `EventLoop` contract. Document ownership in comments when you add state.
- `metrics.h` and `memory_profiler.h` are instrumentation surfaces. Keep them
  lock-free and off the hot path.

Do not add a new header by hand and expect it to build: headers are included
explicitly (`include_directories(include)`), but new `.c` files need a
`cmake -S . -B .` pass.

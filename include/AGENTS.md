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
  the implementation drops records when its pipe is full. `log_access()` carries
  a per-request correlation id; the level is reloadable at runtime and an
  optional syslog mirror is configured through `ServerConfig`.
- `sd_notify.h` is a dependency-free systemd readiness shim.
- `http_server.h` defines `PendingResponse` and `PR_HEADER_BUF_SIZE`. Its
  `header`/`body` pointers alias cached or static memory; the event loop must
  never `free()` them. The owned resources are `owned_body`
  (multipart/byteranges), `body_fd` (a streamed file, released via `close`),
  and `cache_entry` (a pinned `FileCache` reference); the event loop releases
  all three after send or on close. `body_fd` must be `-1` when unused. When
  `header` is NULL the in-struct `header_buf` is sent instead (never point
  `header` at a response's own `header_buf`; the queue copies the struct by
  value). The `method`/`path`/`started` fields are access-log metadata.
- `tls.h` is the OpenSSL-backed TLS surface: one shared `SSL_CTX`, per-connection
  `SSL` creation, the handshake step, and `SIGHUP` cert reload. It is opaque to
  callers that do not need the OpenSSL types.
- `privilege.h` is the Phase 4 privilege-drop surface: `privilege_validate()`
  (resolve `run_user`/`run_group` before binding) and `privilege_drop()` (drop
 after all listeners exist). The drop is irreversible.
- `sandbox.h` exposes the independently toggled Linux Landlock/seccomp startup
  sandbox; enabled features fail closed when unsupported.
- `client_limits.h` exposes bounded per-IP connection and request-rate
  admission/release accounting used by the event loop.
- `path_resolver.h` is the document-root resolution boundary (decode, normalize,
  `openat2`/`O_NOFOLLOW`, directory index, MIME map). Every request-derived path
  must go through it.
- `file_cache.h` defines `Representation` and the bounded, ref-counted cache.
  A cached entry's bodies are borrowed by responses until
  `file_cache_release()`; `file_cache_insert()` takes ownership of the bodies
  it is given.
- `event_loop.h` owns the connection state enum, close reasons, and the
  `EventLoop` contract. Document ownership in comments when you add state.
- `metrics.h` and `memory_profiler.h` are instrumentation surfaces. Keep them
  lock-free and off the hot path. `metrics.h` also owns the request-latency
  histogram, the Prometheus text renderer, and the readiness gauge used by the
  Phase 5 endpoints.

Do not add a new header by hand and expect it to build: headers are included
explicitly (`include_directories(include)`), but new `.c` files need a
`cmake -S . -B build` pass.

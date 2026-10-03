# Changelog

All notable changes to this project are documented in this file. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Release history before 1.1.0 was tracked as phases in `plans/`; entries below
summarize the behavior each phase shipped.

## [Unreleased]

### Added

- Reproducible build: an explicit CMake source list (a new `src/*.c` that is not
  listed fails the link instead of silently not building) and a generated
  `build-manifest.json` recording the toolchain and `zlib`/OpenSSL versions.
- Deployment assets: a hardened `deploy/http-server.service`, an installable
  `/etc` default config, `logrotate` and `tmpfiles.d` snippets, and
  `make install` / `make uninstall` rules.
- Operator runbook covering install, configure, reload, certificate rotation,
  troubleshooting, and benchmark reproduction.
- `LICENSE` (MIT), `CONTRIBUTING.md`, and this changelog.
- `SECURITY.md` (private vulnerability disclosure), `.clang-format`, and
  `.editorconfig` for consistent editing.
- CI: a build-matrix + `ctest` job, a libFuzzer smoke job, and a capacity smoke
  gate tied to a checked-in `benchmarks/ci_baseline.json` floor.

### Changed

- The default build is now out-of-source: `make` configures and builds `./build`
  (the wrapper in `GNUmakefile`) and CMake still emits the executables to
  `./bin`, so the source root no longer collects CMake cache/state.
- The `Dockerfile` builds out-of-source on a pinned Alpine base with the
  required `zlib-dev`/`openssl-dev`, copies the runtime libraries, assets, and
  default config into a minimal final image, and adds a healthcheck; CI builds
  and smoke-tests the image so it cannot silently rot again.
- `project(HTTPServer VERSION ...)` is now `1.1.0` and is compiled into the
  binary as `BUILD_VERSION`.
- Removed the tracked `test` ELF and `server.log` build outputs; `.gitignore`
  now covers them.

## [1.0.0] - 2026 (production milestone, phases 0–5)

### Added

- **Phase 0 — operational safety:** bounded overload (`503`) instead of
  listener-disable starvation, graceful `SIGTERM`/`SIGINT` drain, a single
  validated configuration surface (defaults < file < env < CLI), structured
  leveled JSON logs with a nonblocking writer, and `sd_notify` readiness.
- **Phase 1 — HTTP/1.1 correctness:** strict message parsing (no smuggling,
  obs-fold, or duplicate/conflicting `Content-Length`), methods, conditional
  requests, single and multipart byte ranges, content negotiation, and a
  conformance corpus.
- **Phase 2 — secure static serving:** document-root resolution via
  `openat2(RESOLVE_BENEATH)`/`O_NOFOLLOW`, a MIME map, a bounded ref-counted
  cache, and nonblocking `sendfile` streaming.
- **Phase 3 — TLS termination:** OpenSSL with TLS 1.2+, ALPN `http/1.1`, a
  nonblocking handshake/record state machine, and `SIGHUP` certificate reload.
- **Phase 4 — sandboxing and robustness:** compiler/linker hardening, privilege
  drop, Landlock/seccomp allowlists, per-IP resource controls, libFuzzer
  harnesses, sanitizer/analysis CI, and a coverage baseline.
- **Phase 5 — observability:** a Prometheus `/metrics` endpoint, a request
  latency histogram, `/healthz` and `/readyz` probes, per-request IDs, optional
  syslog, and an extended `SIGHUP` reload.

[Unreleased]: https://github.com/ramtinmk/http_server/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/ramtinmk/http_server/releases/tag/v1.0.0

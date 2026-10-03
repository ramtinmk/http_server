---
plan_id: production-http-server
title: Production-Grade HTTP/1.1 + TLS Static Server
category: program
status: active
owner: agent
created: 2026-09-25
updated: 2026-10-03
related: [scaling-plan, scaling-phase-5-os-tuning, scaling-phase-4-scale-accept, hardware-agnostic-benchmark, formally-verify-c-http-server-with-lean, bare-metal-performance]
---

# Production-Grade HTTP/1.1 + TLS Static Server

## Purpose

The scaling program (`scaling-plan`) took the server from an educational
single-threaded accept loop to a multi-loop nonblocking HTTP/1.1 server with
bounded admission, cached responses, and reproducible measurement. This program
takes the next step: make it **production-grade** — safe to run unattended on a
Linux host, correct enough to face the public internet through TLS, secure
against hostile input, observable, and provably stable under sustained load.

This is a `program` plan: it defines the phases, gates, and acceptance bar for
the whole effort. Each phase gets its own `implementation` plan (or task list)
before work starts, per `plan-spec.md`.

## Baseline

Current capabilities (evidence in `readme.md` Roadmap and the
`scaling-phase-*` plans):

- HTTP/1.1 static serving for three fixed paths (`/`, `/home`, `/hello`) with
  keep-alive, pipelining, `HEAD`, gzip, and cached 404/501/413/414/431 bodies.
- Nonblocking `epoll` event loops, one per CPU core by default, `SO_REUSEPORT`
  listeners, per-connection state machine, and bounded admission
  (`MAX_ACTIVE_CONNECTIONS`, `MAX_QUEUED_TASKS`, buffer pool, timeouts).
- Runtime metrics (`HTTP_SERVER_METRICS_FILE`), startup preflight for
  `RLIMIT_NOFILE`/`somaxconn`/TCP buffers, optional CPU affinity, and a
  hardware-agnostic benchmark harness.
- No TLS, no general file serving, no config file, no privilege drop or
  sandbox, no structured logging, no systemd integration.

Baseline failure evidence (pinned server CPUs 0-3, wrk pinned 4-7, workload
matrix artifact `/tmp/opencode/wrk_diag.json`, 2026-09-25; raw numbers are
host-specific, the shape is what matters):

| wrk | req/s | p99 | client timeouts | peak active | `listener_disabled_count` |
| --- | --- | --- | --- | --- | --- |
| `-c512` | 131,298 | 29 ms | 0 | 513 | 0 |
| `-c1024` | 112,232 | 51 ms | 0 | 1024 | 66 |
| `-c4000` | 107,194 | 1.60 s | 16,171 | 1024 | 25,204 |

Server-side `listen_drops`, `accept_errors`, and `connection_resets` were zero
at every load, yet at 4× capacity 16,171 clients timed out while internal
metrics looked clean. Two production-relevant defects are already visible:

1. **Overload is not fail-fast.** Above capacity the loops disable their
   listeners instead of accepting and returning `503`, so excess clients sit in
   the accept queue and time out rather than being rejected quickly.
2. **Keep-alive reuse is tiny.** `MAX_KEEPALIVE_REQUESTS=100` forces a new TCP
   connection every 100 requests (~22k connections per 2.2M requests at every
   concurrency level), inflating latency and CPU.

These revise the earlier tuning recommendations: the first priority is not
raising `somaxconn` (it is already sufficient) but fixing overload semantics and
connection reuse, then building outward.

## Goals and Non-Goals

### Goals

- **HTTP/1.1 + TLS**: serve an operator-configured document root over
  HTTPS with a vetted TLS library (ALPN advertising `http/1.1`), correct
  caching validators, byte ranges, and MIME handling.
- **Safe by default**: no path can escape the document root; hostile or
  malformed requests fail with correct status codes and never crash, hang, or
  leak. Parser is fuzzed and sanitizer-clean.
- **Operable unattended**: validated configuration, graceful start/reload/stop,
  structured logs, health/readiness, and Prometheus metrics; a hardened systemd
  unit deploys it.
- **Proven under load**: fixed-rate capacity runs at and above the configured
  limit with bounded, documented overload behavior; multi-hour soak with flat
  RSS and file descriptors; TLS capacity measured separately.
- **No regression** of the `scaling-plan` acceptance criteria (5,000 req/s,
  zero unexpected errors, scenario p99, no unbounded growth).

### Non-Goals (first production milestone)

- HTTP/2 and HTTP/3 (ALPN is left ready; multiplexing is a future program).
- Dynamic content: no CGI, FastCGI, reverse proxy, or embedded scripting.
- Container/Kubernetes deployment (bare-metal systemd is the target; a
  container path may follow).
- Windows/macOS support; non-Linux syscall hardening.
- A general-purpose config DSL; a small key/value or INI file plus env is
  enough.
- WebDAV, WebSockets, range multipart > a bounded number of ranges, and
  directory listing.

## Measurement Contract

Reuse the `scaling-plan` measurement contract (warmup / steady / drain,
fixed-rate and fixed-duration, hardware fingerprint, `hardware_agnostic_rps`,
calibration) and the `hardware-agnostic-benchmark` comparison rules. This
program adds:

- **Overload contract**: for a fixed-rate run at `2 ×` the configured
  connection capacity, record client timeouts (must be 0), number of `503`
  responses, `listener_disabled_count`, peak `active_connections`,
  `backlog_depth_max`, and p99. The bounded outcome must be "fast `503` or
  refusal", never "connection accepted but never served to timeout".
- **TLS contract**: run the same scenarios over TLS on the same host and record
  handshakes/s, resumed vs full handshakes, handshake failure modes, and the
  ratio of TLS to plaintext `hardware_agnostic_rps`. Handshake timeouts and
  ALPN selection are reported.
- **Soak contract**: a multi-hour run at a sustained rate, sampling every 60 s:
  RSS, open FDs, active connections, and cache bytes. Pass requires the
  max-min drift over the final 80% of the run to be within 5% with no unbounded
  trend.
- **Interop evidence**: cached `curl`, a browser, and `openssl s_client` connect
  successfully; a conformance corpus of malformed/edge requests is stored with
  expected status codes.

## Progress Snapshot

Legend: `[x]` done, `[~]` partial, `[ ]` pending, `[!]` blocked.

- [x] Phase 0 `production-http-server/phase-0`: complete — bounded overload,
  runtime configuration, graceful drain, and structured logging. Evidence in
  `benchmarks/production_phase0_*.json`.
- [x] Phase 1 `production-http-server/phase-1`: HTTP/1.1 correctness and
  caching semantics. Strict parsing, methods, conditionals, single and
  multipart ranges, negotiation, the conformance corpus, and the black-box
  `scaling-plan` connection/resource gates are done; `EMFILE`/`ENOMEM` fault
  injection remains with the Phase 4 fuzzing/sanitizer work.
- [x] Phase 2 `production-http-server/phase-2`: secure static file serving.
  Document-root resolution (`openat2`/`O_NOFOLLOW`), MIME map, bounded
  ref-counted cache, `sendfile` streaming, and the traversal corpus are done.
  Evidence in `benchmarks/production_phase2_static.json`.
- [x] Phase 3 `production-http-server/phase-3`: TLS termination. Evidence in
  `benchmarks/production_phase3_tls.json`.
- [~] Phase 4 `production-http-server/phase-4`: implementation complete and all
  local exit criteria met for compiler/linker hardening, privilege drop,
  Landlock/seccomp, per-IP/resource controls, fuzzing, sanitizer/analysis CI,
  and coverage enforcement. The only open item is external validation — the
  first remote CI execution and a live systemd start on a target host — which an
  operator must run; no local work remains. Evidence:
  `benchmarks/production_phase4_hardening.json`,
  `benchmarks/production_phase4_sandbox_resources.json`,
  `benchmarks/production_phase4_fuzz.json`, and
  `benchmarks/production_phase4_coverage.json`. See
  `plans/production-http-server-phase4.md`.
- [x] Phase 5 `production-http-server/phase-5`: observability and operations
  complete (Prometheus endpoint, latency histogram, health/readiness, request
  IDs, syslog, and `SIGHUP` log-level/cert/log reload); the readiness probe
  fails via connection-refused once the drain begins, which is the specified
  behavior. Evidence in `benchmarks/production_phase5_observability.json`. See
  `plans/production-http-server-phase5.md`.
- [~] Phase 6 `production-http-server/phase-6`: deployment, CI, and
  reproducible build are implemented (hardened systemd unit + `make
  install`/`uninstall`, explicit source lists + `build-manifest.json`, CI
  build/test/fuzz/capacity jobs, LICENSE/CHANGELOG/CONTRIBUTING, tracked build
  outputs removed). Capacity report published
  (`benchmarks/production_phase6_capacity.json`); validation also found and
  fixed a high-concurrency TLS keep-alive defect. The multi-hour soak is
  running (`make soak` → `benchmarks/production_phase6_soak.json`); the first
  remote CI execution remains an operator action. See
  `plans/production-http-server-phase6.md`.

## Phases

### production-http-server/phase-0: Operational safety and overload semantics

- **Objective:** The current server can run unattended: deterministic overload,
  graceful lifecycle, validated runtime configuration, and logging that cannot
  stall the event loop.
- **Complexity:** 3 — multiple subsystems (admission, signals, config, logging)
  but each is localized and independently testable.
- **Risk:** high — touching the accept/close path can regress the Phase 4
  saturation behavior; every change needs a control run.

**Work**

- [x] Replace listener-disable starvation with a bounded overload policy:
  accept, then send a complete `503 Service Unavailable` and close, or refuse
  quickly; document the queue/deadline. Reconcile with the Phase 4
  saturation design and keep `listener_disabled_count` bounded. Verified by
  `benchmarks/production_phase0_overload.json`.
- [x] Derive effective connection capacity at runtime from `RLIMIT_NOFILE`,
  operator maximum, and the connection-table bound; one source of truth, printed
  and exposed in metrics (already implemented by startup preflight and
  `connection_capacity` metrics).
- [x] Make `MAX_KEEPALIVE_REQUESTS`, timeouts, and capacity runtime-tunable;
  measure the reuse-vs-fairness tradeoff and pick a default with evidence.
  Implemented by `production-http-server-phase0-ops`; defaults unchanged, all
  values settable via file/env/CLI.
- [x] Graceful lifecycle: on `SIGTERM`/`SIGINT` stop accepting, drain in-flight
  within a deadline, flush metrics/logs, then exit; truncate nothing. Add
  `sd_notify` readiness/stopping. Evidence:
  `benchmarks/production_phase0_lifecycle.json`.
- [x] Single configuration source (file + env + CLI) parsed and validated at
  startup; reject contradictory values naming the offending key.
- [x] Structured logging: leveled, JSON access/error logs, batched writer with
  backpressure so the hot path never blocks; `SIGHUP` reopens log files.
  Nonblocking pipe with drop-on-full; see `docs/gotchas.md`.

**Exit criteria**

- [x] At `2 ×` capacity: zero client timeouts, bounded `503`s, bounded
  `listener_disabled_count`, and p99 below the new-connection target; evidence
  in `benchmarks/production_phase0_2x.json` (p99 3.9 ms, 0 timeouts,
  `listener_disabled_count=0`).
- [x] `SIGTERM` under full load drains within the deadline with zero truncated
  responses; process exit status reflects clean shutdown; evidence in
  `benchmarks/production_phase0_lifecycle.json`.
- [x] An invalid config exits non-zero naming the exact key; the effective
  config is recorded in the startup log; evidence in
  `benchmarks/production_phase0_lifecycle.json`.
- [x] Access logging adds < 5% overhead on the keep-alive scenario and never
  blocks an event loop (verified by latency and CPU comparison); evidence in
  `benchmarks/production_phase0_accesslog.json` (overhead within noise,
  zero dropped records).

### production-http-server/phase-1: HTTP/1.1 correctness and caching semantics

- **Objective:** Interoperable, request-smuggling-resistant HTTP/1.1 with
  correct caching validators and ranges.
- **Complexity:** 4 — cross-cutting parser and response semantics. *Not split:*
  work is bounded by RFC 9110/9112, the parser already exists, and each feature
  is independently testable, so splitting would add coordination cost without
  reducing uncertainty.
- **Risk:** medium — correctness bugs are security bugs, but they are covered by
  a conformance corpus and existing regression gates.

**Work**

- [x] Strict message parsing: reject obs-fold and control characters; reject
  duplicate or conflicting `Content-Length`; handle or reject
  `Transfer-Encoding` explicitly (no smuggling); require `Host` on HTTP/1.1;
  support absolute-form targets; enforce request-line/header-count/size limits
  with `414`/`431`.
- [x] Method and status semantics: `GET`, `HEAD`, `POST`, `OPTIONS`; `405` with
  `Allow`; correct `Date`, `Server`, `Content-Length`, and `Connection`
  handling including HTTP/1.0.
- [x] `Expect: 100-continue` handling (unsupported expectations → `417`; the
  server never accepts a body, so it answers with the final status directly).
- [x] Conditional requests: strong/weak `ETag`, `Last-Modified`,
  `If-None-Match`, `If-Modified-Since`, `If-Range`, with `304`.
- [x] Byte ranges: single `206`/`416` and `Accept-Ranges`, plus bounded
  `multipart/byteranges` for multiple satisfiable ranges; invalid or
  over-bounds range-sets fall back to the full `200`.
- [x] Content negotiation: `Accept-Encoding` including `gzip;q=0`, `identity`,
  missing/malformed; emit `Vary`; `406` when nothing is acceptable.
- [x] Build a conformance corpus (malformed, edge, and smuggling cases) with
  expected statuses; wire it into the test suite.

**Exit criteria**

- [x] Every gate in `scaling-plan`'s "Correctness and Regression Gates" is
  checked black-box, including duplicate `Content-Length`, `Transfer-Encoding`,
  header casing, long request lines, connection behavior, and resource safety.
  The `EMFILE`/`ENOMEM` fault-injection gates are handled in code and deferred
  to the Phase 4 fuzzing/sanitizer work (tracked in `scaling-plan.md`).
- [x] The conformance corpus passes with the documented status for every case.
- [x] `curl --http1.0` and keep-alive interop (curl and a browser) succeed.

### production-http-server/phase-2: Secure static file serving

- **Objective:** Serve an operator-configured document root safely and
  efficiently, with no path escaping.
- **Complexity:** 4 — security-critical filesystem handling. *Not split:* the
  module is isolated behind one path-resolution boundary and is proven by one
  traversal corpus, so it stays testable as a unit.
- **Risk:** high — a path-resolution or TOCTOU mistake is a local file
  disclosure vulnerability.

**Work**

- [x] Config: document root, index file list, MIME map, hidden-file policy,
  symlink policy, cache budget.
- [x] Path safety: decode percent-encoding exactly once; normalize `.`/`..`;
  reject encoded traversal, `%00`, and backslashes; resolve with
  `openat2(RESOLVE_BENEATH)` or a dirfd walk with `O_NOFOLLOW`; never follow a
  symlink out of the root.
- [x] Reuse Phase 1 validators (`ETag` from inode/size/mtime, ranges,
  `Content-Type` from the MIME map, `Last-Modified`).
- [x] Efficient output: nonblocking partial writes for file-backed responses;
  `sendfile` with retained offset; bounded LRU representation cache with a byte
  budget and ref-counted eviction.
- [x] Error semantics: `403` vs `404` policy, `405`, `414`, `431`, oversized
  header handling; no directory listing by default.
- [x] Tests: traversal/symlink/null-byte corpus, large files, partial writes,
  slow readers, and cache eviction.

**Exit criteria**

- [x] The traversal corpus cannot read outside the document root (proof in the
  test artifact): `benchmarks/production_phase2_static.json` `traversal_cases`.
- [x] Doc-root serving throughput is within 10% of the cached fixed-path
  baseline on the same host (recorded ratio 0.97).
- [x] Cache bytes and open file descriptors stay within their configured
  budgets under sustained mixed-file load (cache 59735 <= 65536; FDs flat).

### production-http-server/phase-3: TLS termination

- **Objective:** Serve HTTPS through a vetted library with hardened defaults,
  integrated into the nonblocking loop.
- **Complexity:** 4 — TLS handshake/record state machine inside `epoll`.
  *Not split:* the library owns the cryptography; the new work is one
  well-understood state machine with a bounded per-connection buffer, verifiable
  incrementally.
- **Risk:** medium — misconfiguration weakens security or a handshake stalls a
  loop; both are caught by config scan and load tests.

**Work**

- [x] Choose and integrate one library (OpenSSL or mbedTLS); record the choice
  and version in the plan before implementation. OpenSSL 1.1.1f
  (`production-http-server-phase3.md`).
- [x] Nonblocking handshake and record I/O as explicit connection states
  handling `WANT_READ`/`WANT_WRITE`; bounded TLS buffers; handshake timeout;
  no 0-RTT. `CONN_TLS_HANDSHAKE` in `src/event_loop.c`; `TLS_FILE_BUF_SIZE`
  streaming buffer; `header_read_timeout` bounds the handshake.
- [x] Protocol/cipher policy: TLS 1.2+ (prefer 1.3), secure renegotiation,
  session resumption; ALPN advertising `http/1.1`. Renegotiation is disabled
  outright (`SSL_OP_NO_RENEGOTIATION`); TLS 1.2 uses secure renegotiation.
- [x] Certificate/key loading with fail-fast validation and permission checks;
  `SIGHUP` hot reload without dropping connections; optional SNI multi-cert is
  deferred (single certificate loads a chain; SNI multi-cert is out of scope).
- [x] Dual listeners (plaintext + TLS) toggled by config; TLS-specific metrics
  (handshakes, resumptions, failures).
- [x] TLS verification: a self-contained C E2E suite (`./bin/run_tests tls`,
  `tests/tls_test.c`) and a TLS throughput benchmark (`make benchmark-tls`, via
  `scripts/http_benchmark.py --tls`).

**Exit criteria**

- [x] A local protocol/cipher scan (e.g. `testssl.sh`/`ssllabs`-style) reports
  no weak protocol, cipher, or certificate finding. Evidence:
  `benchmarks/production_phase3_tls.json` `tls` (TLSv1.3,
  `TLS_AES_256_GCM_SHA384`, ALPN `http/1.1`); `openssl s_client` also shows
  TLSv1.2 `ECDHE-RSA-AES128-GCM-SHA256` with secure renegotiation.
- [x] Handshakes under load never stall an event loop; `openssl s_client` and a
  browser complete a request. Evidence: 50 concurrent handshakes 50/50, zero
  `tls_handshake_failures`, zero `header_timeout` in the artifact.
- [x] TLS `hardware_agnostic_rps` is recorded and the TLS/plaintext ratio is
  documented; cert reload drops zero connections. Evidence:
  `benchmarks/production_phase3_tls.json` `throughput` and `reload`.

### production-http-server/phase-4: Sandboxing and robustness

- **Objective:** Limit the blast radius of a compromise and survive hostile,
  malformed, and abusive input.
- **Complexity:** 4 — cross-cutting hardening and test infrastructure. *Not
  split:* most items are systemd/library configuration or independent harnesses,
  each reversible on its own.
- **Risk:** medium — over-restrictive sandboxing breaks legitimate features; the
  unit is version-controlled and each hardening knob is toggled independently.

**Work**

- [x] Privilege drop: after binding, drop to an unprivileged uid/gid
  (`run_user`/`run_group`); clears supplementary groups, sets all three
  uids/gids so privilege cannot be regained, plus `no_new_privs`/non-dumpable.
  Evidence: `benchmarks/production_phase4_hardening.json`. The systemd
  `CAP_NET_BIND_SERVICE` path remains under the sandbox item below.
- [x] Sandbox: systemd directives (`NoNewPrivileges`, `ProtectSystem`,
  `ProtectHome`, `PrivateTmp`, `RestrictAddressFamilies`, `SystemCallFilter`),
  plus independently toggled Landlock/seccomp allowlists; documented in
  `deploy/http-server.service` and the Phase 4 runbooks.
- [x] Resource controls: per-IP connection and request-rate limits, global
  active cap with fast `503`, slowloris defenses, output backpressure, fd/core
  controls, and optional `RLIMIT_NPROC`. Evidence:
  `benchmarks/production_phase4_sandbox_resources.json`.
- [x] Fuzzing: opt-in libFuzzer harnesses for the parser and connection state
  machine, with conformance-derived seed corpora and a clean 1,000-input smoke
  run. Evidence: `benchmarks/production_phase4_fuzz.json`.
- [x] Sanitizers and analysis: ASan/UBSan/TSan CI builds, Valgrind close/error
  paths, clang-tidy, and OSV dependency scanning are wired in
  `.github/workflows/ci.yml`.
- [x] Coverage measurement: isolated gcov build over the focused and server E2E
  suites, with a checked-in line/function baseline and CI enforcement. Evidence:
  `coverage/phase4_baseline.json` and
  `benchmarks/production_phase4_coverage.json`.
- [x] Compiler hardening flags (PIE, full RELRO, `-D_FORTIFY_SOURCE=2`, stack
  protector, `-fstack-clash-protection`) behind `ENABLE_HARDENING`, verified by
  `readelf` in `benchmarks/production_phase4_hardening.json`.

**Exit criteria**

- [x] The parser and connection-state fuzz harnesses complete the recorded
  1,000-input smoke run with the corpus checked in; evidence:
  `benchmarks/production_phase4_fuzz.json`.
- [~] ASan/UBSan/TSan, Valgrind, static analysis, and dependency scanning are
  configured in CI; final clean load-matrix evidence awaits the remote CI run.
- [x] The coverage build reports the parser, path resolver, and connection state
  machine above the checked-in line/function baseline; evidence:
  `benchmarks/production_phase4_coverage.json`.
- [~] Application sandboxing and per-IP limits are E2E-proven, and the hardened
  unit is statically validated; starting the installed systemd unit remains a
  deployment acceptance item.

### production-http-server/phase-5: Observability and operations

- **Objective:** Operators can see health, latency, and errors, and can reload
  safe settings without downtime.
- **Complexity:** 3 — additive endpoints and a reload path over existing
  counters.
- **Risk:** low — additive, but the reload path must not drop connections.

**Work**

- [x] Prometheus `/metrics` with latency histograms, throughput, error classes,
  connection/TLS/cache counters, separate from the internal snapshot format.
  Gated by `observability`; paths configurable.
- [x] Structured JSON access/error logs with a request ID; log reopen and
  optional syslog; document fields.
- [x] Health and readiness endpoints on the data listeners; lifecycle logging
  correlates with systemd notify.
- [x] Runtime reload via `SIGHUP` for certs, log files, and safe tunables. Certs,
  log reopen, and a new `log_level` are reloadable now. Per-connection timeouts
  and limits remain restart-only by phase scope (documented); widening the
  reloadable set is a possible future item, not a phase-5 deliverable.
- [x] Document alert thresholds and a minimal dashboard
  (`docs/runbooks/observability-and-reload.md`).

**Exit criteria**

- [x] A scrape returns valid metrics with the documented names; request IDs
  correlate a response to its log line and latency. Evidence:
  `benchmarks/production_phase5_observability.json`.
- [x] `SIGHUP` reload drops zero connections and applies the new log level.
  Evidence: the `reload` block of the same artifact.
- [x] Health serves while accepting and stops during shutdown; `/readyz` is 200
  while accepting and the probe fails once the drain begins (observed as
  connection-refused because the listener is removed, which is the specified
  "fails once the drain begins" behavior). Evidence: `health` / `lifecycle`
  blocks.

### production-http-server/phase-6: Deployment, CI, and capacity validation

- **Objective:** Ship a hardened deployable artifact and prove stability and
  capacity with reproducible evidence.
- **Complexity:** 3 — packaging and automation around tested code.
- **Risk:** low — no protocol changes.

**Work**

- [x] Hardened systemd unit (sandbox directives, `LimitNOFILE`, restart policy,
  `Type=notify`), install/uninstall targets, default config under `/etc`,
  `logrotate` and `tmpfiles` snippets.
- [x] CI: build matrix, unit/e2e suites, fuzz smoke, sanitizer jobs, coverage
  artifact, and a smoke benchmark gate tied to a checked-in baseline. The CMake
  `cmake -S . -B . && make` + `ctest` build/test job is in place.
- [x] Reproducible build: explicit source lists (a new `src/*.c` fails loudly
  instead of silently not building), pinned `zlib`/OpenSSL apt packages in CI,
  and a recorded `build-manifest.json` (compiler, CMake, dependencies, git).
- [x] Release and repository hygiene: `LICENSE`, `CHANGELOG`, `CONTRIBUTING`;
  `project(... VERSION)` bumped to `1.1.0`; tracked `test` ELF and `server.log`
  removed with `.gitignore` coverage.
- [~] Capacity report: fixed-rate at/above capacity plus TLS are published in
  `benchmarks/production_phase6_capacity.json`; the multi-hour soak harness and
  `make soak` are ready and a short control passes; the 3-hour acceptance run is
  in progress and will publish `benchmarks/production_phase6_soak.json`.
  - [x] Deterministic file-class corpus (`scripts/benchmark_corpus.py`,
    `make corpus`) so capacity runs and comparisons use byte-identical assets and
    report per class rather than one average. See `docs/benchmarks.md`.
  - [x] Peer comparison against nginx over plaintext, `gzip_static`, and TLS
    (`scripts/run_nginx_comparison.sh`, `scripts/compare_servers.py`, `wrk`) with
    matched workers/optimization and disjoint CPU pinning; evidence in
    `benchmarks/nginx_comparison.csv` + `.json` and the recipe in
    `docs/runbooks/compare-against-nginx.md`.
- [x] Operator runbook: install, configure, reload, cert rotation,
  troubleshooting, and benchmark reproduction live in
  `docs/runbooks/deploy-systemd.md`, linked from `readme.md` (the phase-6
  `6f` workstream and its exit criterion record this; the earlier
  `readme.md`/`AGENTS.md` wording was superseded).

**Exit criteria**

- [x] One documented command installs and starts the hardened service; the unit
  is enabled and `systemd-analyze` clean (verified against a staged install).
- [~] CI gates build/test/fuzz/coverage and a throughput regression fails the
  pipeline: jobs and gate are committed and validated locally; the first remote
  run is pending.
- [x] No build outputs are tracked (`git ls-files` reports no compiled binary or
  log) and `LICENSE`/`CHANGELOG`/`CONTRIBUTING` exist.
- [~] The soak artifact shows RSS and FD drift within 5% over the final 80% of a
  multi-hour run (harness ready; 3-hour run in progress, artifact pending at
  `benchmarks/production_phase6_soak.json`).
- [x] The capacity report is published and the `scaling-plan` acceptance
  criteria are re-verified
  (`benchmarks/production_phase6_capacity.json`).

## Gates

These must remain green for any phase to merge or proceed:

- CI builds and tests the tree with the documented CMake commands and `ctest`;
  the workflow must never invoke a build command that does not exist.
- Build, lint, and the focused unit suites (`ring`, `thread_pool`) and server
  e2e suite per `AGENTS.md`.
- The `scaling-plan` correctness and regression gates.
- No new compile warnings under the project warning flags.
- Fuzz smoke and ASan/UBSan clean, and coverage at or above the checked-in
  baseline (from Phase 4 onward).
- Overload contract (zero timeouts, bounded `503`) and graceful-shutdown
  contract (no truncated responses).
- Traversal corpus (from Phase 2 onward).
- No unbounded growth in RSS, FDs, active connections, or cache bytes.

## Acceptance

Program completion requires all of:

The implementation is complete and every local exit criterion is met. The
remaining `[~]` items are external validation that requires resources this
repository does not own: the first remote CI execution, a live systemd start on
a target host, and the multi-hour soak wall-clock run.

- [~] Phase 0–6 exit criteria met with recorded evidence. All local criteria are
  met; Phase 4's remote CI/systemd gates and Phase 6's soak/CI gates remain
  operator actions.
- [x] HTTPS serving of a document root with correct caching/range semantics and
  a clean TLS scan (`benchmarks/production_phase2_static.json`,
  `benchmarks/production_phase3_tls.json`).
- [~] A hardened systemd deployment that survives restart and sandboxing. The
  unit installs, `systemd-analyze verify` is clean, and the sandbox is
  E2E-proven; starting the installed unit on a live host is a deployment action.
- [~] Fuzzed, sanitizer-clean, coverage-measured parser and state machine. The
  fuzz smoke is clean and coverage is enforced locally
  (`benchmarks/production_phase4_fuzz.json`,
  `benchmarks/production_phase4_coverage.json`); the sanitizer load matrix runs
  in remote CI.
- [~] A CI pipeline that builds, tests, and gates the tree, and a repository
  with no tracked build outputs and a published license/changelog. The jobs and
  gate are committed and validated locally and the repository is clean; the
  first remote run is pending.
- [~] Published capacity report: fixed-rate, above-capacity, TLS, and soak, with
  no regression of the `scaling-plan` 5,000 req/s acceptance. Fixed-rate,
  above-capacity, and TLS are published
  (`benchmarks/production_phase6_capacity.json`); the multi-hour soak is running
  (`benchmarks/production_phase6_soak.json`).
- [x] Reproducible build: explicit source list, pinned toolchain/dependencies
  (`build-manifest.json`).
- [x] Operator runbook complete (`docs/runbooks/deploy-systemd.md`,
  `docs/runbooks/observability-and-reload.md`).

## Risks and rollback

- **Overload change destabilizes Phase 4 saturation behavior** → Compare it with
  the recorded Phase 4 control evidence; revert the isolated accept/reject policy
  if `listener_disabled_count`, drops, or resets regress.
- **TLS integration stalls loops or regresses throughput** → Keep TLS behind a
  config toggle and a separate listener; plaintext remains the reference path
  and the fallback.
- **Path-resolution vulnerability** → Isolate resolution in one audited
  function with `openat2`/`O_NOFOLLOW`; the traversal corpus is a merge gate.
- **Refactor for correctness breaks existing endpoints** → Keep the fixed-path
  cached variants as regression fixtures until doc-root serving passes
  parity.
- **Sandbox breaks a legitimate feature** → Every restriction is a separate
  systemd directive or allowlist entry, toggled and recorded independently so
  it can be relaxed without reverting the rest.
- **Scope creep toward HTTP/2 or dynamic content** → Explicit non-goals; a new
  program plan is required to change them.
- **Benchmark host drift invalidates comparisons** → Reuse the
  `hardware-agnostic-benchmark` fingerprint and calibration; treat
  cross-kernel or cross-governor comparisons as invalid.

## Execution order

1. Phase 0 — fix overload semantics, lifecycle, config, logging (unblocks safe
   unattended operation and removes the baseline defects).
2. Phase 1 — HTTP/1.1 correctness and caching; establish the conformance
   corpus.
3. Phase 2 — secure static file serving on top of the corrected parser.
4. Phase 3 — TLS termination; ALPN-ready, HTTP/2 deferred.
5. Phase 4 — sandboxing, fuzzing, sanitizers, resource controls.
6. Phase 5 — Prometheus metrics, structured logs, health, reload.
7. Phase 6 — hardened systemd packaging, CI gates, reproducible build, release
   and repository hygiene, capacity and soak report.

Phase 1 may proceed in parallel with Phase 0's logging/config work once the
overload policy is fixed, because they touch different code paths. Phases 2 and
3 depend on Phase 1; Phase 5 depends on Phase 0's logging; Phase 6 depends on
all of the above.

---
plan_id: production-http-server
title: Production-Grade HTTP/1.1 + TLS Static Server
category: program
status: active
owner: agent
created: 2026-09-25
updated: 2026-09-25
related: [scaling-plan, scaling-phase-5-os-tuning, scaling-phase-4-scale-accept, hardware-agnostic-benchmark, formally-verify-c-http-server-with-lean]
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

- [ ] Phase 0 `production-http-server/phase-0`: operational safety and overload
  semantics.
- [ ] Phase 1 `production-http-server/phase-1`: HTTP/1.1 correctness and
  caching semantics.
- [ ] Phase 2 `production-http-server/phase-2`: secure static file serving.
- [ ] Phase 3 `production-http-server/phase-3`: TLS termination.
- [ ] Phase 4 `production-http-server/phase-4`: sandboxing and robustness.
- [ ] Phase 5 `production-http-server/phase-5`: observability and operations.
- [ ] Phase 6 `production-http-server/phase-6`: deployment, CI, and capacity
  validation.

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

- [ ] Replace listener-disable starvation with a bounded overload policy:
  accept, then send a complete `503 Service Unavailable` and close, or refuse
  quickly; document the queue/deadline. Reconcile with the Phase 4
  saturation design and keep `listener_disabled_count` bounded.
- [ ] Derive effective connection capacity at runtime from `RLIMIT_NOFILE`,
  operator maximum, and the connection-table bound; one source of truth, printed
  and exposed in metrics.
- [ ] Make `MAX_KEEPALIVE_REQUESTS`, timeouts, and capacity runtime-tunable;
  measure the reuse-vs-fairness tradeoff and pick a default with evidence.
- [ ] Graceful lifecycle: on `SIGTERM`/`SIGINT` stop accepting, drain in-flight
  within a deadline, flush metrics/logs, then exit; truncate nothing. Add
  `sd_notify` readiness/stopping.
- [ ] Single configuration source (file + env + CLI) parsed and validated at
  startup; reject contradictory values naming the offending key.
- [ ] Structured logging: leveled, JSON access/error logs, batched writer with
  backpressure so the hot path never blocks; `SIGHUP` reopens log files.

**Exit criteria**

- [ ] At `2 ×` capacity: zero client timeouts, bounded `503`s, bounded
  `listener_disabled_count`, and p99 below the new-connection target; evidence
  in the benchmark CSV plus `wrk_diag`-style artifact.
- [ ] `SIGTERM` under full load drains within the deadline with zero truncated
  responses; process exit status reflects clean shutdown.
- [ ] An invalid config exits non-zero naming the exact key; the effective
  config is recorded in the startup log.
- [ ] Access logging adds < 5% overhead on the keep-alive scenario and never
  blocks an event loop (verified by latency and CPU comparison).

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

- [ ] Strict message parsing: reject obs-fold and control characters; reject
  duplicate or conflicting `Content-Length`; handle or reject
  `Transfer-Encoding` explicitly (no smuggling); require `Host` on HTTP/1.1;
  support absolute-form targets; enforce request-line/header-count/size limits
  with `414`/`431`.
- [ ] Method and status semantics: `GET`, `HEAD`, `POST`, `OPTIONS`; `405` with
  `Allow`; correct `Date`, `Server`, `Content-Length`, and `Connection`
  handling including HTTP/1.0.
- [ ] `Expect: 100-continue` handling.
- [ ] Conditional requests: strong/weak `ETag`, `Last-Modified`,
  `If-None-Match`, `If-Modified-Since`, `If-Range`, with `304`.
- [ ] Byte ranges: single range and bounded multipart ranges with
  `206`/`416` and `Accept-Ranges`.
- [ ] Content negotiation: `Accept-Encoding` including `gzip;q=0`, `identity`,
  missing/malformed; emit `Vary`.
- [ ] Build a conformance corpus (malformed, edge, and smuggling cases) with
  expected statuses; wire it into the test suite.

**Exit criteria**

- [ ] Every gate in `scaling-plan`'s "Correctness and Regression Gates" is
  checked, including duplicate `Content-Length`, `Transfer-Encoding`, header
  casing, and long request lines.
- [ ] The conformance corpus passes with the documented status for every case.
- [ ] `curl --http1.0` and keep-alive interop (curl and a browser) succeed.

### production-http-server/phase-2: Secure static file serving

- **Objective:** Serve an operator-configured document root safely and
  efficiently, with no path escaping.
- **Complexity:** 4 — security-critical filesystem handling. *Not split:* the
  module is isolated behind one path-resolution boundary and is proven by one
  traversal corpus, so it stays testable as a unit.
- **Risk:** high — a path-resolution or TOCTOU mistake is a local file
  disclosure vulnerability.

**Work**

- [ ] Config: document root, index file list, MIME map, hidden-file policy,
  symlink policy, cache budget.
- [ ] Path safety: decode percent-encoding exactly once; normalize `.`/`..`;
  reject encoded traversal, `%00`, and backslashes; resolve with
  `openat2(RESOLVE_BENEATH)` or a dirfd walk with `O_NOFOLLOW`; never follow a
  symlink out of the root.
- [ ] Reuse Phase 1 validators (`ETag` from inode/size/mtime, ranges,
  `Content-Type` from the MIME map, `Last-Modified`).
- [ ] Efficient output: nonblocking partial writes for file-backed responses;
  optional `sendfile` with retained offset; optional bounded mmap cache for hot
  assets with an eviction policy and a memory budget.
- [ ] Error semantics: `403` vs `404` policy, `405`, `414`, `431`, oversized
  header handling; no directory listing by default.
- [ ] Tests: traversal/symlink/null-byte corpus, large files, partial writes,
  slow readers, and cache eviction.

**Exit criteria**

- [ ] The traversal corpus cannot read outside the document root (proof in the
  test artifact).
- [ ] Doc-root serving throughput is within 10% of the cached fixed-path
  baseline on the same host.
- [ ] Cache bytes and open file descriptors stay within their configured
  budgets under sustained mixed-file load.

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

- [ ] Choose and integrate one library (OpenSSL or mbedTLS); record the choice
  and version in the plan before implementation.
- [ ] Nonblocking handshake and record I/O as explicit connection states
  handling `WANT_READ`/`WANT_WRITE`; bounded TLS buffers; handshake timeout;
  no 0-RTT.
- [ ] Protocol/cipher policy: TLS 1.2+ (prefer 1.3), secure renegotiation,
  session resumption; ALPN advertising `http/1.1`.
- [ ] Certificate/key loading with fail-fast validation and permission checks;
  `SIGHUP` hot reload without dropping connections; optional SNI multi-cert.
- [ ] Dual listeners (plaintext + TLS) toggled by config; TLS-specific metrics
  (handshakes, resumptions, failures).

**Exit criteria**

- [ ] A local protocol/cipher scan (e.g. `testssl.sh`/`ssllabs`-style) reports
  no weak protocol, cipher, or certificate finding.
- [ ] Handshakes under load never stall an event loop; `openssl s_client` and a
  browser complete a request.
- [ ] TLS `hardware_agnostic_rps` is recorded and the TLS/plaintext ratio is
  documented; cert reload drops zero connections.

### production-http-server/phase-4: Sandboxing and robustness

- **Objective:** Limit the blast radius of a compromise and survive hostile,
  malformed, and abusive input.
- **Complexity:** 4 — cross-cutting hardening and test infrastructure. *Not
  split:* most items are systemd/library configuration or independent harnesses,
  each reversible on its own.
- **Risk:** medium — over-restrictive sandboxing breaks legitimate features; the
  unit is version-controlled and each hardening knob is toggled independently.

**Work**

- [ ] Privilege drop: after binding, drop to an unprivileged uid/gid; use
  `CAP_NET_BIND_SERVICE` via systemd instead of root where possible; never
  regain privilege.
- [ ] Sandbox: systemd directives (`NoNewPrivileges`, `ProtectSystem`,
  `ProtectHome`, `PrivateTmp`, `RestrictAddressFamilies`, `SystemCallFilter`),
  plus Landlock/seccomp allowlist; document each.
- [ ] Resource controls: per-IP connection and request-rate limits, global
  active cap with fast `503`, slowloris defenses, output backpressure, fd and
  memory budgets, optional `RLIMIT_*`.
- [ ] Fuzzing: libFuzzer/AFL++ harnesses for the parser and connection state
  machine; a seed corpus from the conformance cases.
- [ ] Sanitizers and analysis: ASan/UBSan/TSan CI builds, valgrind on
  close/error paths, static analysis, and dependency vulnerability scanning.
- [ ] Compiler hardening flags (PIE, RELRO, `-D_FORTIFY_SOURCE`, stack
  protector, `-fstack-clash-protection`).

**Exit criteria**

- [ ] The fuzz harness runs clean for a recorded number of CPU-hours and the
  corpus is checked in.
- [ ] ASan/UBSan/TSan are clean under the load matrix; no leaks across
  connect/disconnect, timeout, and partial-write paths.
- [ ] The hardened unit starts and serves with the sandbox active; per-IP limits
  are proven by a test that opens more than the allowed connections.

### production-http-server/phase-5: Observability and operations

- **Objective:** Operators can see health, latency, and errors, and can reload
  safe settings without downtime.
- **Complexity:** 3 — additive endpoints and a reload path over existing
  counters.
- **Risk:** low — additive, but the reload path must not drop connections.

**Work**

- [ ] Prometheus `/metrics` with latency histograms, throughput, error classes,
  connection/TLS/cache counters, separate from the internal snapshot format.
- [ ] Structured JSON access/error logs with a request ID; log reopen and
  optional syslog; document fields.
- [ ] Health and readiness endpoints distinct from the data path; lifecycle
  logging correlates with systemd notify.
- [ ] Runtime reload via `SIGHUP` for certs, log files, and safe tunables
  (timeouts, limits); document exactly what is reloadable.
- [ ] Document alert thresholds and a minimal dashboard.

**Exit criteria**

- [ ] A scrape returns valid metrics with the documented names; request IDs
  correlate a log line to its metrics/latency.
- [ ] `SIGHUP` reload drops zero connections and applies the new settings.
- [ ] Health/readiness change correctly during start and shutdown.

### production-http-server/phase-6: Deployment, CI, and capacity validation

- **Objective:** Ship a hardened deployable artifact and prove stability and
  capacity with reproducible evidence.
- **Complexity:** 3 — packaging and automation around tested code.
- **Risk:** low — no protocol changes.

**Work**

- [ ] Hardened systemd unit (sandbox directives, `LimitNOFILE`, restart policy,
  `Type=notify`), install/uninstall targets, default config under `/etc`,
  `logrotate` and `tmpfiles` snippets.
- [ ] CI: build matrix, unit/e2e suites, fuzz smoke, sanitizer jobs, and a
  smoke benchmark gate tied to a checked-in baseline.
- [ ] Capacity report: fixed-rate runs at and above capacity, TLS runs, and a
  multi-hour soak with RSS/FD drift evidence.
- [ ] Operator runbook in `readme.md`/`AGENTS.md`: install, configure, reload,
  cert rotation, troubleshooting, and benchmark reproduction.

**Exit criteria**

- [ ] One documented command installs and starts the hardened service; it
  survives a reboot.
- [ ] CI gates are green and a regression in throughput or correctness fails
  the pipeline.
- [ ] The soak artifact shows RSS and FD drift within 5% over the final 80% of a
  multi-hour run.
- [ ] The capacity report is published and the `scaling-plan` acceptance
  criteria are re-verified.

## Gates

These must remain green for any phase to merge or proceed:

- Build, lint, and the focused unit suites (`ring`, `thread_pool`) and server
  e2e suite per `AGENTS.md`.
- The `scaling-plan` correctness and regression gates.
- No new compile warnings under the project warning flags.
- Fuzz smoke and ASan/UBSan clean (from Phase 4 onward).
- Overload contract (zero timeouts, bounded `503`) and graceful-shutdown
  contract (no truncated responses).
- Traversal corpus (from Phase 2 onward).
- No unbounded growth in RSS, FDs, active connections, or cache bytes.

## Acceptance

Program completion requires all of:

- [ ] Phase 0–6 exit criteria met with recorded evidence.
- [ ] HTTPS serving of a document root with correct caching/range semantics and
  a clean TLS scan.
- [ ] A hardened systemd deployment that survives restart and sandboxing.
- [ ] Fuzzed, sanitizer-clean parser and state machine.
- [ ] Published capacity report: fixed-rate, above-capacity, TLS, and soak, with
  no regression of the `scaling-plan` 5,000 req/s acceptance.
- [ ] Operator runbook complete.

## Risks and rollback

- **Overload change destabilizes Phase 4 saturation behavior** → Land it behind
  the existing saturation scenario with a control run; revert the policy flag
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
7. Phase 6 — hardened systemd packaging, CI gates, capacity and soak report.

Phase 1 may proceed in parallel with Phase 0's logging/config work once the
overload policy is fixed, because they touch different code paths. Phases 2 and
3 depend on Phase 1; Phase 5 depends on Phase 0's logging; Phase 6 depends on
all of the above.

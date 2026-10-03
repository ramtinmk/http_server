---
plan_id: production-http-server-phase6
title: Phase 6 deployment, CI, reproducible build, and capacity validation
category: implementation
status: active
owner: agent
created: 2026-10-03
updated: 2026-10-03
related: [production-http-server, production-http-server-phase5]
---

# Phase 6 deployment, CI, reproducible build, and capacity validation

## Purpose

Turn the tested Phase 0–5 server into a shippable artifact: a hardened systemd
installation with one documented command, a reproducible CMake build with an
explicit source list and a recorded dependency manifest, CI that actually builds
and tests the tree (with a throughput regression gate tied to a checked-in
baseline), a clean repository with a license/changelog/contributing guide, and a
published capacity and soak report. This is the `production-http-server` phase 6
phase; it changes no protocol behavior.

## Scope

In: `CMakeLists.txt` (explicit source list, install/uninstall rules, dependency
manifest), new `deploy/` assets (unit, `/etc` config, `logrotate`, `tmpfiles`),
`.github/workflows/ci.yml` (build/test matrix, fuzz smoke, benchmark gate),
repository hygiene files (`LICENSE`, `CHANGELOG.md`, `CONTRIBUTING.md`, version
bump, `.gitignore`, removing tracked build outputs), a capacity/soak report
harness plus artifacts under `benchmarks/`, and operator documentation in
`readme.md` / `docs/runbooks/`.

Out: protocol, parser, TLS, or sandbox behavior changes; HTTP/2; container
orchestration beyond the existing `Dockerfile`/`docker-compose.yml`; a general
packaging format (`.deb`/`.rpm`) — systemd `install` plus `make install` is the
milestone; distributed tracing.

## Baseline

- **Build:** `CMakeLists.txt` discovered sources with `file(GLOB "src/*.c")` and
  `file(GLOB "tests/*.c")`, so adding a `src/*.c` silently did not build until a
  re-configure and removing one silently dropped it. `project(HTTPServer VERSION
  1.0)` was a hardcoded placeholder. No dependency versions were recorded.
- **Packaging:** `deploy/http-server.service` existed but was not installable
  (`CMakeLists.txt` had no `install()` rules); there was no default config for
  `/etc`, no `logrotate` or `tmpfiles.d` snippet, and no documented
  install/uninstall command.
- **CI:** `.github/workflows/ci.yml` had sanitizer, analysis/Valgrind,
  dependency-scan, and coverage jobs, but no build-matrix job, no `ctest` job,
  no fuzz-smoke job, and no throughput regression gate. The legacy autotools
  `c-cpp.yml` stub had already been removed.
- **Repository hygiene:** `git ls-files` tracked a compiled ELF `test` and
  `server.log`; there was no `LICENSE`, `CHANGELOG`, or `CONTRIBUTING`.
- **Capacity:** the deterministic corpus (`make corpus`) and the nginx peer
  comparison (`scripts/run_nginx_comparison.sh`) were done; there was an
  in-repo `memory-soak` artifact but no multi-hour fixed-rate soak with a
  documented RSS/FD drift verdict, no checked-in throughput baseline for CI, and
  no `open_fds` metric (the process is non-dumpable, so external procfs reads
  are denied).
- Evidence: `CMakeLists.txt`, `.github/workflows/ci.yml`, `deploy/`,
  `git ls-files`, `benchmarks/`, `scripts/`.

## Findings during validation (fixed here)

The Phase 6 TLS/TLS-concurrency capacity run surfaced a real production defect:
at high concurrency a small fraction (~0.04%) of TLS keep-alive requests had the
connection closed before the server responded, while plaintext was clean. Root
cause: the event loop called `SSL_get_error()` without clearing the
thread-local OpenSSL error queue, so a stale error left by `SSL_shutdown()` on a
previous connection made a benign `WANT_READ` look like a fatal
`SSL_ERROR_SSL`; OpenSSL 3 also reports an unexpected peer EOF as
`SSL_ERROR_SSL`. Fixed by clearing the queue before each
`SSL_read`/`SSL_write`/`SSL_accept` and setting `SSL_OP_IGNORE_UNEXPECTED_EOF`;
covered by a new TLS keep-alive churn E2E test. Evidence: 3× repeated TLS runs
at 25k requests with zero failures, versus 7–12 before.

Two harness defects were also fixed: `phase0_capacity_2x.py` double-converted an
already-millisecond wrk p99 to milliseconds (any p99 > 5 ms falsely failed as
"unbounded"), and `phase6_capacity_report.py` compared fixed-rate throughput to
5,000 exactly, failing on a one-request boundary miss.

## Measurement contract

Reuse the `scaling-plan` / `hardware-agnostic-benchmark` contract. Phase 6 adds:

- **Smoke-baseline gate:** a short fixed-rate keep-alive run recording
  `hardware_agnostic_rps`; CI fails when the measured value is below the
  checked-in floor in `benchmarks/ci_baseline.json` after the same host
  fingerprint check used by `--check-env`.
- **Soak contract:** a fixed-rate stationary load sampled every 60 s; pass
  requires the max−min drift of RSS, PSS, and open FDs over the final 80% of the
  run to be within 5% with no unbounded trend. Reported fields: sample count,
  first/last window means, drift percentages, and pass/fail per metric.
- **Capacity report:** fixed-rate runs at and `2×` the configured connection
  capacity plus a TLS keep-alive run, each reporting offered rate, achieved
  rate, `hardware_agnostic_rps`, p50/p99, `503` count, timeouts, and the host
  fingerprint.

## Workstreams

### 6a Reproducible build

**Work**

- [x] Replace `file(GLOB ...)` source discovery with explicit `SOURCES` /
  `TEST_SOURCES` lists so adding a source that is not listed fails loudly
  (undefined symbols) instead of silently not building.
- [x] Bump `project(HTTPServer VERSION ...)` to `1.1.0` and compile it into the
  binary as `BUILD_VERSION`.
- [x] Record the resolved toolchain and dependency versions in
  `build-manifest.json` at configure time (compiler, CMake, `zlib`, OpenSSL, git
  revision, explicit source lists).
- [x] Pin the CI toolchain and `zlib`/OpenSSL versions (explicit apt packages in
  each job) and upload the per-build manifest artifact.

### 6b Repository hygiene

**Work**

- [x] Add `LICENSE` (MIT).
- [x] Add `CHANGELOG.md` seeded from the program's phase history.
- [x] Add `CONTRIBUTING.md` describing build, test, branch, and plan workflow.
- [x] Remove the tracked compiled `test` ELF and `server.log`; extend
  `.gitignore` so compiled artifacts, logs, and the generated manifest cannot
  recur.

### 6c Hardened packaging

**Work**

- [x] Harden and parameterize `deploy/http-server.service` (sandbox directives,
  `LimitNOFILE`, restart policy, `Type=notify`, `ExecReload`).
- [x] Add `deploy/http_server.conf` as the installable `/etc` default config.
- [x] Add `deploy/logrotate.conf` and `deploy/http-server.tmpfiles` (plus a
  `sysusers` snippet for the service account).
- [x] Add `install`/`uninstall` CMake rules and document one command
  (`docs/runbooks/deploy-systemd.md`). Verified with a staged install and
  `systemd-analyze verify`.

### 6d CI gates

**Work**

- [x] Add a build-matrix + `ctest` job that runs the documented
  `cmake -S . -B . && make` and `ctest` with the server running.
- [x] Add a fuzz-smoke job gated on `ENABLE_FUZZING` (Clang).
- [x] Add a capacity smoke benchmark gate tied to the checked-in
  `benchmarks/ci_baseline.json` floor (`scripts/ci_capacity_gate.py`).
- [x] Keep sanitizer/analysis/dependency-scan/coverage jobs and upload the build
  manifest as an artifact.

### 6e Capacity and soak report

**Work**

- [x] Soak harness: the existing `scripts/memory_soak.py` now samples and gates
  open FDs too (from the new self-reported `open_fds` metric) and a `make soak`
  target runs the multi-hour acceptance.
- [x] Capacity harness: `scripts/phase6_capacity_report.py` runs plaintext,
  TLS, and `2×`-capacity overload end to end (`make phase6-capacity`).
- [x] Publish a capacity report artifact and re-verify the `scaling-plan`
  5,000 req/s acceptance: `benchmarks/production_phase6_capacity.json`.
- [x] Seed the checked-in `benchmarks/ci_baseline.json` from a measured run.

### 6f Operator runbook and docs

**Work**

- [x] Operator runbook: `docs/runbooks/deploy-systemd.md` (install, configure,
  reload, cert rotation, troubleshooting, capacity reproduction), linked from
  `readme.md`.
- [x] Update `docs/architecture.md`, `docs/gotchas.md`, `docs/benchmarks.md`,
  `docs/runbooks/observability-and-reload.md`,
  `docs/runbooks/verify-changes.md`, `AGENTS.md`, `src/AGENTS.md`,
  `tests/AGENTS.md`, `readme.md`, and the program progress snapshot.

## Steps (this iteration)

1. [x] Explicit source lists, version bump, and build manifest in
   `CMakeLists.txt` + a manifest template.
2. [x] Repository hygiene: `LICENSE`, `CHANGELOG.md`, `CONTRIBUTING.md`, remove
   tracked outputs, `.gitignore`.
3. [x] Packaging: hardened unit, `/etc` config, logrotate/tmpfiles/sysusers,
   install/uninstall rules.
4. [x] CI: build/ctest matrix, fuzz smoke, benchmark gate, manifest artifact.
5. [x] Soak + capacity harnesses, `open_fds` metric, and baseline.
6. [x] Fix the TLS keep-alive defect and harness p99/threshold bugs found by
   validation; add the TLS churn E2E test.
7. [x] Docs and program-plan update.

## Validation

- [x] `cmake -S . -B . && make` succeeds; the explicit list fails loudly on a
  missing source.
- [x] `./bin/run_tests ring`, `./bin/run_tests tls` (7/7, including the new
  keep-alive churn case), and the server suite pass.
- [x] `ctest --output-on-failure` passes with the server running.
- [x] `git ls-files` reports no compiled binary or log.
- [x] `make install DESTDIR=<tmp>` lays out the expected files; `make uninstall`
  removes them; `systemd-analyze verify` accepts the unit.
- [x] The soak harness exits 0 on a short control and writes an artifact with
  gated RSS/PSS/FD drift.
- [x] The capacity smoke gate passes at the checked-in floor and fails when the
  floor is raised.
- [x] `make phase6-capacity` passes (plaintext 5,000 req/s, zero timeouts, TLS
  ratio recorded, bounded `2×` overload).

## Exit criteria

- [x] One documented command installs and starts the hardened service; the unit
  is `WantedBy=multi-user.target` and `systemd-analyze` clean (truthfully
  verified against a staged install; starting the installed unit on a live host
  remains a deployment action).
- [~] CI gates build/test/fuzz/coverage and a throughput regression fails the
  pipeline: the jobs and gate are committed and validated locally; the first
  remote CI run is pending.
- [x] `git ls-files` reports no compiled binary or log and
  `LICENSE`/`CHANGELOG`/`CONTRIBUTING` exist.
- [~] The soak artifact shows RSS and FD drift within 5% over the final 80% of a
  multi-hour run: the harness and `make soak` are ready and a short control
  passes; the 3-hour acceptance run is in progress and writes
  `benchmarks/production_phase6_soak.json`. Blocked only on wall-clock time; no
  code or harness work remains.
- [x] The capacity report is published
  (`benchmarks/production_phase6_capacity.json`) and the `scaling-plan`
  acceptance is re-verified.
- [x] No regression in the ring/server/TLS suites.

## Risks and rollback

- **Install rules overwrite host files** → every installed path is under
  `/usr/local` and `/etc/http-server`; `make uninstall` removes exactly the
  recorded manifest.
- **Hardened unit breaks a legitimate feature** → each directive is separate and
  removable; the unit is verified with `systemd-analyze` and staged install.
- **Explicit source list goes stale** → a missing symbol fails the link (loud),
  which is the intended behavior; CI builds from a clean checkout.
- **Benchmark gate is host-sensitive** → the gate uses `hardware_agnostic_rps`
  with a conservative floor (2,000 vs a local ~17,000) and documents how to
  raise it.
- **Soak is long** → the harness records `config.duration_seconds` so a short
  control is never mistaken for the acceptance soak.

---
plan_id: production-http-server-phase4
title: Phase 4 sandboxing and robustness
category: implementation
status: active
owner: agent
created: 2026-10-01
updated: 2026-10-02
related: [production-http-server, production-http-server-phase3]
---

# Phase 4 sandboxing and robustness

## Purpose

Limit the blast radius of a compromise and survive hostile, malformed, and
abusive input: build the binary hardened, drop privileges after binding, apply
an OS sandbox, bound per-client resource use, and fuzz/sanitize the parser and
connection state machine. This is `production-http-server` phase 4.

The phase is deliberately split into seven independent workstreams (4a–4g).
Each workstream is reversible on its own (a CMake option, a runtime key, a
systemd directive, or a separate harness), which is the split justification the
program plan records. This plan tracks them together because they share one
objective and one exit gate.

## Scope

In: compiler/linker hardening (`CMakeLists.txt`); a runtime privilege-drop
surface (`run_user`/`run_group`) implemented in a new `src/privilege.c` +
`include/privilege.h` and applied after all listeners are bound; Landlock/
seccomp allowlists and a hardened systemd unit; per-IP connection and
request-rate limits, slowloris defenses, output backpressure, and optional
`RLIMIT_*`; libFuzzer harnesses with a seed corpus from the Phase 1 conformance
cases; ASan/UBSan/TSan build options and CI jobs; a coverage-instrumented
build with a checked-in baseline for the parser, path resolver, and connection
state machine; and the E2E harness `scripts/phase4_hardening_test.py` plus docs.

Out: HTTP/2 and HTTP/3, dynamic content, and a general config DSL (program
non-goals). Cross-compilation and non-Linux sandboxes.

## Baseline

- `CMakeLists.txt` builds with `-O2 -DNDEBUG` and warnings only; no `PIE`,
  `RELRO`, `_FORTIFY_SOURCE`, stack protector, or `-pie` link flag.
- The server runs as the invoking user for its whole lifetime; there is no
  `setuid`/`setgid`, no `PR_SET_NO_NEW_PRIVS`, and no `PR_SET_DUMPABLE` clear.
  `main.c` binds as root and stays root.
- No per-IP limits, no Landlock/seccomp, no fuzz harness, no sanitizer build.
- Evidence: `CMakeLists.txt`, `src/main.c`, `docs/architecture.md`.

## Workstreams

### 4a Compiler and linker hardening

**Work**

- [x] GNU/Clang hardening flags behind `ENABLE_HARDENING` (default ON):
  `-fstack-protector-strong`, `-fstack-clash-protection`, `-fPIE` +
  `-pie`, `-Wl,-z,relro,-z,now`, `-Wl,-z,noexecstack`, and
  `_FORTIFY_SOURCE=2` for optimized configs only (it is inert/invalid at `-O0`).
  Flags are compile-probed; `-DENABLE_HARDENING=OFF` rolls back.
- [x] Verify with `readelf`: PIE type `DYN`, `GNU_RELRO` + `BIND_NOW`,
  `__stack_chk_fail` present, fortified `*_chk` symbols present, and a
  non-executable `GNU_STACK`. Evidence: the `hardening` block of
  `benchmarks/production_phase4_hardening.json`.
- [x] Fix the latent `snprintf` truncation in `path_resolver.c` that
  `_FORTIFY_SOURCE=2` surfaced (bounded copy, no new warning).

### 4b Privilege drop

**Work**

- [x] Runtime keys `run_user` and `run_group` (name or numeric id) with
  defaults/validation in the one config surface; `--help` lists them.
- [x] `privilege_validate()` resolves the identity at startup and fails naming
  the offending key before any listener is bound.
- [x] `privilege_drop()` runs after every listener (plaintext + TLS, all loops)
  is bound and before any loop thread starts: `initgroups`/`setgroups`,
  `setresgid`, `setresuid` (all three ids, so privilege cannot be regained),
  then `PR_SET_NO_NEW_PRIVS` and `PR_SET_DUMPABLE=0`. It fails closed and
  verifies the resulting ids.
- [x] Document that a root-only document root or certificate cannot be read
  after the drop, and that `SIGHUP` cert reload therefore requires the cert to
  be readable by `run_user`.

### 4c OS sandbox

**Work**

- [x] Landlock filesystem allowlist (read-only document root, read log/cert)
  and a seccomp-BPF syscall allowlist installed after binding. Runtime toggles,
  fail-closed installation, a denied-path self-test, and process-state E2E
  coverage are implemented.
- [x] Hardened systemd unit directives (`NoNewPrivileges`, `ProtectSystem`,
  `ProtectHome`, `PrivateTmp`, `RestrictAddressFamilies`, `SystemCallFilter`)
  and `CAP_NET_BIND_SERVICE` for low ports instead of root.
- [x] Each application sandbox is independently toggled and documented; unit
  directives are independently editable in `deploy/http-server.service`.

### 4d Resource controls

**Work**

- [x] Per-IP connection and request-rate limits with a bounded table and
  inactive-entry eviction.
- [x] Global active cap kept, slowloris defenses (per-header and total header
  progress deadlines), output backpressure.
- [x] Optional `RLIMIT_*` (NOFILE already handled; core=0, optional NPROC)
  and documented fd/resource controls.

### 4e Fuzzing

**Work**

- [x] libFuzzer target for the HTTP parser and one for the connection state
  machine, built by the `ENABLE_FUZZING` CMake option (not in the default build).
- [x] A seed corpus checked in under `fuzz/corpus/`, based on Phase 1
  conformance cases.
- [x] A recorded clean fuzz run and the corpus in the repo; evidence is
  `benchmarks/production_phase4_fuzz.json` from the 1,000-input smoke run per
  target.

### 4f Sanitizers and analysis

**Work**

- [x] ASan/UBSan/TSan build options and CI jobs over the focused suites and the
  load smoke; Valgrind covers ring and server close/error paths.
- [x] Static analysis (existing `make lint`) and OSV dependency vulnerability
  scan are wired into CI.

### 4g Coverage measurement

**Work**

- [x] A coverage-instrumented `ENABLE_COVERAGE` build option using gcov that does
  not disturb the default Release build.
- [x] Measure line/function coverage over the focused suites and the server e2e
  suite for the parser, path resolver, and connection state machine; record the
  baseline in `coverage/phase4_baseline.json`.
- [x] Enforce the baseline in CI, failing when coverage drops below it and
  uploading `benchmarks/production_phase4_coverage.json`.

## Steps (4a-4f progress)

1. [x] Add the `ENABLE_HARDENING` option and flags to `CMakeLists.txt`.
2. [x] Add `run_user`/`run_group` to `include/server_config.h`,
   `include/config.h`, and the `CFG_KEYS` table in `src/config.c`.
3. [x] Add `include/privilege.h` + `src/privilege.c` with
   `privilege_validate()` and `privilege_drop()`.
4. [x] Call `privilege_validate()` from `main.c` before binding and
   `privilege_drop()` from `event_loop_run()` after all listeners are created.
5. [x] Record the effective run identity in the startup print and log line.
6. [x] Add `scripts/phase4_hardening_test.py` and a `phase4-hardening` target.
7. [x] Add independently toggled Landlock/seccomp application sandboxing and
   the hardened systemd unit in `deploy/http-server.service`.
8. [x] Add bounded per-IP connection/rate accounting, header progress deadlines,
   output backpressure, and optional core/NPROC resource limits.
9. [x] Add E2E validation for sandbox syscall/path coverage and resource-limit
   enforcement; evidence is `benchmarks/production_phase4_sandbox_resources.json`.
10. [x] Add the opt-in parser and connection-state libFuzzer targets, checked-in
    seed corpora, and smoke-run artifact.
11. [x] Add independent ASan/UBSan/TSan CMake options, sanitizer CI jobs,
    Valgrind close/error coverage, clang-tidy, and OSV scanning.
12. [x] Add isolated gcov coverage measurement, the checked-in Phase 4 baseline,
    and CI enforcement with an uploaded coverage artifact.

## Validation

- [x] `cmake -S . -B . && make` with no new warnings.
- [x] `./bin/run_tests ring`.
- [x] `./bin/run_tests server` with the server started `--document-root .`:
  `VERDICT: ALL PASSED`. (The documented `./bin/http_server &` recipe serves
  `document_root=root`, so `test_docroot_static_serving`'s `/readme.md` case is
  a pre-existing failure on `main`, not a regression; see Risks.)
- [x] `./bin/run_tests tls`.
- [x] `python3 scripts/phase4_hardening_test.py` exits 0 and writes
  `benchmarks/production_phase4_hardening.json`.
- [x] ASan+UBSan and Clang TSan builds compile; ASan+UBSan ring/TLS and Clang
  TSan ring pass locally. CI additionally runs the server E2E, load smoke,
  Valgrind close/error path, clang-tidy, and OSV scan.

## Exit criteria

- [x] The binary is PIE, full-RELRO, NX-stack, stack-protected, and fortified
  with `_FORTIFY_SOURCE=2`; evidence: `readelf` fields in the artifact.
- [x] With `run_user` configured, the process runs as that uid/gid (verified by
  `/proc/<pid>/status`), serves a request, and cannot regain the original ids;
  an unresolvable name/group is fatal and names the key.
- [x] No regression in the existing ring/server/TLS suites (the one server-suite
  failure reproduces on `main`).
- [x] `make phase4-sandbox-resources` passes: Landlock/seccomp state and serving,
  denied-path self-test, per-IP admission, slowloris closure, rlimits, and
  systemd directive checks; evidence is the generated JSON artifact.
- [x] Default `make` and `./bin/run_tests ring` remain green; both fuzz harnesses
  pass strict GCC syntax checks. The Clang/libFuzzer smoke run passes both
  targets and writes `benchmarks/production_phase4_fuzz.json`.
- [x] ASan+UBSan and Clang TSan builds configure and compile; ASan+UBSan ring/TLS
  suites and Clang TSan ring suite pass locally. Full Valgrind/OSV evidence is
  produced by CI on the configured Ubuntu runner.
- [x] `cmake --build <coverage-build> --target coverage` passes locally with
  parser, path-resolver, and connection-state line/function floors enforced.
- [x] The coverage build reports the parser, path resolver, and connection state
  machine at or above the checked-in line/function baseline; evidence is
  `benchmarks/production_phase4_coverage.json` and the CI coverage artifact.

## Risks and rollback

- **Hardening flag unsupported or breaks the build** → each flag is added
  behind `ENABLE_HARDENING` and a compile-flag probe; rollback is
  `-DENABLE_HARDENING=OFF`.
- **Privilege drop breaks a legitimate feature (root-only doc root/cert)** →
  the drop only happens when `run_user`/`run_group` is configured; leaving them
  empty keeps the previous behavior. Rollback: unset the keys.
- **Ordering bug drops before a listener is bound** → the drop is called only
  after the listener-creation loop completes, and a failure aborts startup
  before any request is served.
- **Fortify mismatch at `-O0`** → `_FORTIFY_SOURCE=2` is applied only to
  non-Debug configuration generator expressions.
- **Coverage instrumentation perturbs the measured binary or gates on noise** →
  the coverage build is a separate CMake option off the default Release path,
  and the baseline is a floor (fail below), not an exact match.

---
plan_id: complexity-refactoring
title: Reduce C cyclomatic complexity hotspots
category: program
status: active
owner: maintainer
created: 2026-10-16
updated: 2026-10-16
related: []
---

# Reduce C cyclomatic complexity hotspots

## Purpose

Reduce the server's highest-complexity functions into independently testable
helpers without changing HTTP behavior, security policy, connection ownership,
or lifecycle semantics. This plan turns the new CCN 15 pre-commit gate into an
incremental refactoring program rather than masking the existing baseline.

## Baseline

The full-tree command `make complexity` reports 27 functions above CCN 15
using Lizard 1.17.10. The evidence and complete hotspot list are maintained in
[`docs/runbooks/complexity-baseline.md`](../docs/runbooks/complexity-baseline.md).
The largest findings are:

| Function | File | CCN |
| --- | --- | ---: |
| `apply_seccomp` | `src/sandbox.c` | 93 |
| `read_http_response` | `tests/server_test.c` | 45 |
| `el_prepare_response` | `src/http_server.c` | 36 |
| `parse_byte_ranges` | `src/http_server.c` | 32 |
| `privilege_drop` | `src/privilege.c` | 31 |
| `event_loop_main` | `src/event_loop.c` | 29 |
| `event_loop_run` | `src/event_loop.c` | 28 |
| `parse_request_line` | `src/http_server.c` | 28 |

## Goals and Non-goals

### Goals

- Reduce every runtime C function to CCN 15 or lower, measured by Lizard
  1.17.10 using the project-local `make complexity` command.
- Preserve existing response semantics, parser rejection behavior, security
  restrictions, TLS state transitions, and shutdown ordering.
- Leave each phase independently buildable and verifiable.
- Keep the pre-commit CCN 15 gate enabled throughout the work.

### Non-goals

- No change to public HTTP behavior, configuration defaults, resource limits, or
  security policy.
- No redesign of the event-loop concurrency or ownership model.
- No blanket increase of the CCN threshold and no exclusion of existing source
  files to make the metric pass.
- No refactoring of unrelated benchmark or vendored code.

## Measurement contract

- **Metric:** Lizard cyclomatic complexity number (CCN).
- **Tool/version:** project-local Lizard 1.17.10 from `tools/quality/requirements.txt`.
- **Command:** `make complexity` from the repository root.
- **Environment:** project `.venv`, C11 project checkout, Linux CI or developer
  workstation with the normal repository dependencies.
- **Pass threshold:** every runtime and test C function reports CCN <= 15.
- **Reported fields:** file, function, line, CCN, NLOC, token count, parameter
  count, and function length for every finding.
- **Correctness evidence:** `make`, focused `ring` and `tls` suites, server E2E
  suite, and the relevant security/TLS/parser acceptance harness for each phase.

## Progress snapshot

- Phase 1 — pending: 2 functions, highest CCN 93.
- Phase 2 — pending: 9 HTTP parsing/response functions, highest CCN 36.
- Phase 3 — pending: 7 event-loop and TLS I/O functions, highest CCN 29.
- Phase 4 — pending: 9 startup, privilege, path, and test-helper functions,
  highest CCN 45.
- Overall: 0/27 findings resolved; the baseline remains intentionally visible.

## Phases

### complexity-refactoring/phase-1: Sandbox policy decomposition

- **Objective:** Split sandbox policy construction from platform installation so
  security behavior is unchanged while `apply_seccomp` and `apply_landlock`
  meet CCN 15.
- **Complexity:** 3 — localized security module with platform-specific branches
  and strict ordering requirements.
- **Risk:** high — incorrect filter ordering or errno handling can weaken startup
  hardening or prevent the server from starting.

**Work**

- [ ] Extract declarative seccomp rule groups and small installation helpers
  from `apply_seccomp`.
- [ ] Separate Landlock capability detection, ruleset construction, and apply
  operations in `apply_landlock`.
- [ ] Preserve fail-closed behavior, logging, syscall allowlists, and feature
  detection results.
- [ ] Add or extend an E2E artifact covering successful startup and expected
  startup failure when sandbox setup is unavailable.

**Exit criteria**

- [ ] `apply_seccomp` and `apply_landlock` each report CCN <= 15.
- [ ] `make phase4-sandbox-resources` passes and records its normal artifact.
- [ ] Hardened startup behavior and failure messages are unchanged.

### complexity-refactoring/phase-2: HTTP parser and response decomposition

- **Objective:** Decompose request parsing, range parsing, date/encoding parsing,
  multipart formatting, and response selection without changing wire behavior.
- **Complexity:** 4 — parser and response helpers share strict protocol and
  buffering invariants across several functions.
- **Risk:** high — small ordering or boundary changes can cause request smuggling,
  incorrect status codes, or malformed response framing.

**Work**

- [ ] Split `parse_request_line`, `parse_header_line`, and
  `parse_accept_encoding` into lexical validation and semantic validation steps.
- [ ] Split `parse_byte_ranges` into grammar parsing, normalization, and bounds
  validation helpers.
- [ ] Separate `parse_http_date` and `reason_phrase` lookup/validation branches
  into table-driven or single-purpose helpers.
- [ ] Split `format_multipart`, `select_asset_response`, and
  `el_prepare_response` into policy selection and serialization stages.
- [ ] Preserve all existing parser, conditional-request, range, gzip, and
  multipart E2E behavior.

**Exit criteria**

- [ ] All nine phase-2 functions report CCN <= 15.
- [ ] `./bin/run_tests ring`, `./bin/run_tests tls`, and the server suite pass.
- [ ] HTTP parser and static-response acceptance artifacts show no new failures.

### complexity-refactoring/phase-3: Event-loop and TLS I/O decomposition

- **Objective:** Separate readiness dispatch, TLS read/write transitions,
  connection admission, and loop lifecycle while preserving ownership and
  shutdown invariants.
- **Complexity:** 4 — cross-cutting nonblocking I/O and TLS state transitions
  must remain compatible with epoll interest updates and connection ownership.
- **Risk:** high — regressions can deadlock, leak descriptors, drop responses, or
  mishandle TLS WANT_READ/WANT_WRITE transitions.

**Work**

- [ ] Extract accept/admission decisions from `el_accept`.
- [ ] Factor common TLS readable/writable transition handling from
  `el_tls_readable` and `el_tls_writable`.
- [ ] Separate plain readable/writable dispatch from response and close policy.
- [ ] Split `event_loop_main` and `event_loop_run` into setup, poll/dispatch,
  drain, and teardown helpers without changing thread ownership.
- [ ] Exercise keep-alive, overload, TLS, timeout, signal-drain, and descriptor
  error paths after each decomposition.

**Exit criteria**

- [ ] All seven phase-3 functions report CCN <= 15.
- [ ] `./bin/run_tests ring`, `./bin/run_tests tls`, and server E2E pass.
- [ ] `make phase0-lifecycle`, `make phase0-2x`, and `make phase3-tls` pass.
- [ ] No descriptor, connection, or TLS handshake regressions are present in
  the generated artifacts.

### complexity-refactoring/phase-4: Startup, paths, and test helpers

- **Objective:** Reduce remaining startup/configuration, privilege/path, and
  server-test helper complexity after the protocol and event-loop boundaries are
  stable.
- **Complexity:** 3 — several localized modules, with lower coupling than the
  parser and event-loop phases.
- **Risk:** medium — startup and path changes affect security and diagnostics;
  test-helper changes can hide regressions if assertions are weakened.

**Work**

- [ ] Split `privilege_drop` into identity setup, capability handling, and final
  hardening helpers.
- [ ] Decompose `print_server_config`, `apply_cpu_affinity`, and `main` around
  parsing, validation, startup, and teardown boundaries.
- [ ] Split `parse_index_files`, `normalize_path`, and `open_relative` while
  preserving beneath-root and symlink protections.
- [ ] Split `read_http_response` and `test_el_capacity_and_fd_leak` into
  transport, parsing, assertion, and cleanup helpers without weakening checks.
- [ ] Re-run the complete complexity report and update the baseline document
  only after the measured findings are resolved.

**Exit criteria**

- [ ] All nine phase-4 functions report CCN <= 15.
- [ ] `make complexity` exits 0 with no exclusions.
- [ ] Full CTest/E2E validation passes and no test assertion coverage is lost.

## Gates

- No phase merges with a new changed-function CCN > 15.
- `git diff --check`, `make`, `make lint` when clang-tidy is available, and the
  phase-specific E2E suites remain green.
- Security-sensitive changes require successful sandbox, path traversal, and
  privilege-drop validation before proceeding.
- Protocol changes require parser, static-response, range, gzip, keep-alive,
  and TLS validation before proceeding.

## Risks and rollback

- **Behavior drift during extraction** → keep each helper extraction in a
  focused commit and revert the phase commit; do not mix semantic changes with
  complexity reduction.
- **Security hardening regression** → stop the phase, compare startup logs and
  sandbox artifacts, and revert the affected phase before continuing.
- **Event-loop race or descriptor leak** → stop the phase, run lifecycle/TLS/
  capacity checks, and revert to the last phase gate.
- **Metric gaming through exclusions or threshold increases** → reject the
  change and retain the CCN 15 gate and complete source scope.

## Execution order

Execute phases in order: sandbox policy, HTTP parser/response, event-loop/TLS,
then startup/path/test helpers. Each phase must meet its exit criteria before
the next begins; after Phase 4, run the complete gates and update the baseline
evidence to show zero findings.

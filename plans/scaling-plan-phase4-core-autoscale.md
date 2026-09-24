---
plan_id: scaling-phase-4-core-autoscale
title: Phase 4 Auto-Scale Event Loops to CPU Cores
category: implementation
status: done
owner: agent
created: 2026-09-24
updated: 2026-09-24
related: [scaling-plan, scaling-phase-4-scale-accept, scaling-phase-4-profiling]
---

# Phase 4 Auto-Scale Event Loops to CPU Cores

- **Phase ID:** `scaling-phase-4-core-autoscale`
- **Objective:** Make the default event-loop thread count track the number of online CPU cores at runtime instead of a fixed compile-time constant, while preserving an explicit override.
- **Complexity:** 2 — A few files in one module with one new resolver function and a runtime `SO_REUSEPORT` decision; no new concurrency model, no new interfaces beyond one accessor.
- **Risk:** medium — The default now opens `SO_REUSEPORT` listeners, which changes port-sharing semantics (a second instance can bind the same port instead of failing) and raises default thread/listener counts on large hosts.

## Purpose

The event-loop count was a fixed compile-time constant (`EL_THREAD_COUNT`,
default `4`), so the server ran the same number of loops on a 2-core laptop and a
32-core host. This plan makes the server size itself to the host the way
production event-driven servers do: one event loop per online CPU core by
default, with `0` meaning auto-detect and a positive value remaining an explicit
override. It closes the "choose the production value by measurement around the
available CPU count" intent recorded in `scaling-phase-4-scale-accept` step C.4
without forcing a rebuild per host.

## Scope

**Work locations:** `include/server_config.h`, `include/event_loop.h`,
`include/http_server.h`, `src/event_loop.c`, `src/http_server.c`, `src/main.c`.
Use the build and test conventions in `AGENTS.md`.

**In:**

- A runtime resolver `event_loop_thread_count()` that returns the explicit
  `EL_THREAD_COUNT` when positive, otherwise `sysconf(_SC_NPROCESSORS_ONLN)`
  clamped to `[1, EL_MAX_THREADS]`.
- `event_loop_run` taking the resolved loop count as a parameter rather than
  reading the macro.
- `create_server_socket(int reuseport)` deciding `SO_REUSEPORT` at runtime so the
  listener flag matches the resolved loop count.
- Startup reporting of the resolved loop count.

**Out:**

- Multi-process scaling, a dedicated acceptor, and cross-process accounting;
  those remain deferred by `scaling-phase-4-profiling`.
- CPU affinity and env-pinning; those belong to `scaling-phase-5-os-tuning`.
- Per-host tuning of `EL_MAX_THREADS` beyond the fixed safety clamp.

## Baseline

Before this change:

- `EL_THREAD_COUNT` defaulted to the compile-time value `4`
  (`include/server_config.h:115`), independent of the host; the profiling spike
  text recorded that the default should remain `1` while the code default was
  `4`, an unresolved inconsistency.
- `create_server_socket()` gated `SO_REUSEPORT` on a compile-time
  `#if EL_THREAD_COUNT > 1` (`src/http_server.c:266`), so the listener flag could
  not follow a runtime count.
- `event_loop_run` read `EL_THREAD_COUNT` directly (`src/event_loop.c:882`).

On this 8-core host the default build ran 4 loops regardless of the 8 available
cores.

## Steps

1. [x] Add `EL_MAX_THREADS` and change `EL_THREAD_COUNT` default to `0` (auto)
   with updated documentation in `include/server_config.h`.
2. [x] Add `event_loop_thread_count()` and change `event_loop_run` to accept the
   loop count in `src/event_loop.c` / `include/event_loop.h`.
3. [x] Change `create_server_socket()` to take a `reuseport` flag and set
   `SO_REUSEPORT` at runtime in `src/http_server.c` / `include/http_server.h`.
4. [x] Resolve the count once in `src/main.c`, pass it to the socket and
   `event_loop_run`, and print it at startup.

## Validation

- [x] `make` rebuilds `bin/http_server` and `bin/run_tests` cleanly.
- [x] Start `./bin/http_server` from the repository root and run
  `./bin/run_tests server`; all 24 tests pass.
- [x] Startup output reports the resolved count: on this 8-core host,
  `8 loops`, with per-loop diagnostics showing accepts on all 8 loops.

## Exit criteria

- [x] The default build starts one event loop per online CPU core and prints the
  resolved count at startup; evidence: server startup output
  (`event-loop 0..7` diagnostics and `Dispatch model: ... 8 loops`).
- [x] A positive `EL_THREAD_COUNT` still overrides auto-detection, and
  `EL_THREAD_COUNT=1` preserves the single-loop control.
- [x] `SO_REUSEPORT` is set exactly when the resolved loop count is greater than
  one, and omitted for the single-loop control so a duplicate instance still
  fails to bind.
- [x] `./bin/run_tests server` passes 24/24 with the server running, confirming
  multi-loop accept, serve, keep-alive, and close behavior.

## Risks and rollback

- **Port-sharing semantics change** — With loops > 1 the listener uses
  `SO_REUSEPORT`, so a second server instance silently shares the port rather
  than failing to bind. Rollback: set `EL_THREAD_COUNT=1`, which restores the
  no-`SO_REUSEPORT` single-loop control.
- **Thread/listener growth on large hosts** — Auto-detection on a many-core host
  creates one thread and listener per core. Rollback: set a smaller explicit
  `EL_THREAD_COUNT`, or lower `EL_MAX_THREADS`.
- **Unjustified scaling vs. the profiling gate** — `scaling-phase-4-profiling`
  measured the single loop as not the limiter on its host and chose an opt-in
  default. This plan intentionally changes that default to host-sized; the
  override preserves the profiled control for benchmark comparisons.

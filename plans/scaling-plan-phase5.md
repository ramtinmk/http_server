---
plan_id: scaling-phase-5-os-tuning
title: Phase 5 Operating-System and Deployment Tuning
category: implementation
status: active
owner: agent
created: 2026-09-22
updated: 2026-09-22
related: [scaling-plan, hardware-agnostic-benchmark]
---

# Phase 5 Operating-System and Deployment Tuning

- **Phase ID:** `scaling-phase-5-os-tuning`
- **Objective:** Apply host, kernel, and deployment tuning only after application bottlenecks are addressed, with each change independently reversible and proven by a control run.
- **Complexity:** 3 — Multiple files and scripts are touched, but the work is configuration, startup preflight validation, and measurement plumbing rather than a new concurrency or persistence model.
- **Risk:** medium — Environment tuning can mask an application regression and produce benchmark numbers that are not comparable to earlier runs.

## Purpose

The parent plan lists Phase 5 as tuning that is only worthwhile once the
application is no longer the limit. This plan makes that tuning reproducible and
honest: the server enforces and reports the descriptor and backlog limits it
actually depends on, the benchmark refuses to measure an unconfigured
environment, and every setting is recorded with its kernel version and toggled
off in a control run so the improvement is attributable to the environment and
not to noise. The environment-pinning work is shared with
`hardware-agnostic-benchmark` Phase 4 and must not be duplicated or conflict.

## Scope

**Work locations:** `src/main.c` and `include/server_config.h` for startup
preflight and limit enforcement; `scripts/http_benchmark.py` for the
environment check and additional fingerprint fields; a new
`scripts/run_benchmark_pinned.sh` wrapper; `docker-compose.yml` for the
constant resource envelope; and `readme.md`/`AGENTS.md` for the operator
runbook. Use the build and test conventions in `AGENTS.md`.

**In:**

- Enforcing the required soft `RLIMIT_NOFILE` and failing clearly when it
  cannot be satisfied.
- Verifying `net.core.somaxconn` and TCP send/receive buffer limits, and
  reporting the effective backlog.
- Startup configuration validation (positive values, cross-limit consistency).
- Optional, measured-only CPU affinity configuration.
- Benchmark environment preflight (`--check-env`), a pinned-run wrapper, the
  documented constant resource envelope, and CPU frequency/governor recording.
- Recording every tuning change and reverting it in a control run.

**Out:**

- Application-level concurrency changes; those belong to
  `scaling-phase-4-scale-accept`.
- TLS, dynamic content, and arbitrary-path file serving.
- Automatic sysctl mutation by the server at runtime; the server verifies and
  reports, it does not silently change host state.

## Baseline

The benchmark fingerprint already records `ulimit_nofile_soft`,
`ulimit_nofile_hard`, `os_kernel`, `cpu_model`, `cpu_logical_cores`,
`cpu_physical_cores`, `cpu_mhz`, `cpu_max_mhz`, `memory_total_kb`, and
`page_size_kb` (see `plans/hardware-agnostic-benchmark.md`). The server currently
prints the soft/hard `RLIMIT_NOFILE` and required headroom but only warns when
the soft limit is too low (`check_nofile_limit()` in `src/main.c`); it does not
enforce the limit or verify the effective backlog. `BACKLOG` defaults to 1024 in
`include/http_server.h`, but the kernel `somaxconn` cap is not read or reported.
No CPU governor, TCP buffer, or listener-drop field is recorded, and the
benchmark has no environment preflight.

The control for this phase is a bare run of the Phase 4 configuration on the
documented benchmark host with the settings left at their distribution
defaults; the treatment is the same run under the documented envelope.

## Measurement contract

Reuse the parent plan's contract and reported fields. For each tuning change:

1. Run a control benchmark with the setting at its default.
2. Apply exactly one setting.
3. Rerun the identical scenario and duration.
4. Record both rows with `machine_id`, `os_kernel`, and the setting's value.
5. Revert the setting and confirm the control row is reproduced within the
   existing <5% throughput variation before attributing any gain to the change.

Record at minimum: `somaxconn`, effective backlog, `tcp_rmem`/`tcp_wmem`
limits, CPU governor and frequency, `ulimit_nofile_soft`/`_hard`, open file
descriptors, context switches, TCP retransmits, listen drops, and accept errors
by errno.

## Steps / Work

### A. Server-side preflight and enforcement

1. [ ] In startup, attempt to raise the soft `RLIMIT_NOFILE` up to the hard
   limit with `setrlimit`, and re-read it. Fail with a clear error when the
   effective soft limit is still below `MAX_ACTIVE_CONNECTIONS +
   REQUIRED_NOFILE_HEADROOM`, replacing the current warning-only behavior.
2. [ ] Read `/proc/sys/net/core/somaxconn`, compute the effective backlog as
   `min(BACKLOG, somaxconn)`, print it, and warn or fail when the configured
   `BACKLOG` cannot be honored.
3. [ ] Validate configuration at startup: reject non-positive limits and
   contradictory combinations (for example an admission cap above the effective
   descriptor capacity), and exit non-zero with the offending value named.
4. [ ] Optionally set CPU affinity from an environment variable such as
   `HTTP_SERVER_CPU_SET`, only when a measurement shows scheduler migration
   matters; leave it unset by default and print the applied mask.

### B. Benchmark and deployment automation

1. [ ] Add `--check-env` to `scripts/http_benchmark.py`: verify the CPU
   governor is `performance` (when required), `ulimit_nofile` is sufficient,
   `somaxconn` is at least the configured backlog, expected core counts are
   present, and any requested pinning is in effect. Exit non-zero naming the
   exact unmet requirement instead of producing a noisy run.
2. [ ] Add the environment fields listed in the measurement contract to the
   fingerprint and CSV (`somaxconn`, effective backlog, `tcp_rmem`, `tcp_wmem`,
   governor) as trailing additions, preserving backward compatibility.
3. [ ] Add `scripts/run_benchmark_pinned.sh` wrapping `taskset` for the server
   and the generator, with documented per-host core ranges, and route the
   matrix through it when pinning is enabled.
4. [ ] Add a constant resource envelope to `docker-compose.yml` for the server
   service (`cpus`, `mem_limit`, `pids_limit`) and document that compose runs
   produce envelope-normalized numbers.
5. [ ] Document the required governor command (`cpupower frequency-set -g
   performance` or the sysfs equivalent) and the fixed envelope
   (`THREAD_POOL_SIZE`, matrix `--concurrency`, ulimit, backlog) in
   `readme.md` and/or `AGENTS.md`.

### C. Measurement and recording

1. [ ] Extend the server and benchmark metrics with listener drops and accept
   errors by errno where the platform exposes them, and surface TCP retransmits
   (already recorded) alongside the new fields.
2. [ ] For every tuning change, commit the control and treatment rows with the
   kernel version and the setting value, and revert after measuring.

## Validation

- [ ] Run `make`; run `./bin/run_tests ring` and `./bin/run_tests thread_pool`.
- [ ] Start `./bin/http_server` from the repository root and run
  `./bin/run_tests server`.
- [ ] Start the server with a reduced soft `RLIMIT_NOFILE` (for example
  `ulimit -n 64`) and confirm it fails clearly or reports the unsatisfied
  requirement without crashing.
- [ ] Run `python3 scripts/http_benchmark.py --print-hardware` and
  `--check-env` on the documented host; confirm `--check-env` either passes or
  exits non-zero naming the unmet requirement.
- [ ] Run the benchmark matrix bare and under the documented pin/limit envelope
  and compare `server_cpu_seconds_per_1000_requests`,
  `hardware_agnostic_rps`, and normalized throughput.

## Exit criteria

- [ ] The required soft `RLIMIT_NOFILE` is enforced at startup and reported;
  the server fails clearly when it cannot be satisfied.
- [ ] The effective backlog (`min(BACKLOG, somaxconn)`) and the relevant TCP
  buffer limits are read, reported, and recorded in the benchmark fingerprint.
- [ ] Invalid or contradictory configuration fails startup with the offending
  value named.
- [ ] `--check-env` passes cleanly on the documented host or exits non-zero with
  the exact unmet requirement named.
- [ ] Two matrix runs on the same hardware, one bare and one under the
  documented pin/limit envelope, produce CPU-time metrics and normalized
  throughput within 10% of each other, or the discrepancy is explained in this
  plan.
- [ ] Every tuning change has a recorded control run at the same kernel version,
  and the setting was reverted after measurement.
- [ ] Phase 4 acceptance criteria are not regressed (5,000 req/s, zero
  unexpected errors, scenario-specific p99, no unbounded growth).
- [ ] No new runtime dependency is introduced.

## Risks and rollback

- **Tuning masks an application regression** → Keep the bare control run and
  compare CPU-time and normalized metrics, not raw wall-clock throughput; revert
  the setting if the apparent gain does not persist in the control.
- **Raising descriptors or backlog hides an admission bug** → Enforce the
  application limit independently; the environment change must not be the only
  thing preventing overload.
- **Frequency scaling makes calibration and CPU metrics noisy** → Require the
  performance governor for calibration runs and record the governor; treat
  unrecorded-governor comparisons as invalid.
- **Affinity hurts by isolating the wrong work** → Enable affinity only from a
  measured result and print the applied mask; leave it unset by default so it
  can be removed with one environment change.
- **Duplicate or conflicting env-pinning work with the benchmark plan** →
  Reference `hardware-agnostic-benchmark` Phase 4 as the source of truth for
  pinning and `--check-env`, and keep this plan focused on the server-side
  preflight and the measurement contract.
- **Deployment drift between compose and bare-metal** → Record the envelope with
  every result and state plainly that compose numbers are envelope-normalized.

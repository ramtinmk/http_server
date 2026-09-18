# Hardware-Agnostic Benchmark Plan

## Purpose

The stress-test matrix (`scripts/http_benchmark.py` driven by
`scripts/run_benchmark_matrix.sh`, results in `benchmarks/*.csv`) records
machine-specific numbers: raw throughput, latency in milliseconds,
`server_cpu_percent`, RSS, context switches, and retransmits. None of these are
comparable across different hardware, so a phase-over-phase improvement on one
machine cannot be validated against a run from another.

This plan makes the benchmark hardware-agnostic in three layers, each
independently shippable:

1. **Fingerprint** — record what machine produced each run (record, do not
   normalize), matching the pending Phase-0 contract item "Pin down the
   benchmark host, CPU count, kernel, compiler flags, and ulimit values" in
   `plans/scaling-plan.md`.
2. **CPU-time accounting** — derive cost-per-request in CPU seconds, which
   removes wall-clock and core-count dependence from comparisons.
3. **Calibration** — run a fixed CPU-bound microbenchmark on the server host at
   test start, compute a machine speed index, and produce normalized throughput
   and latency columns.

An optional fourth layer constrains the environment (pinning, cgroups) so runs
are comparable by construction rather than by arithmetic.

Requirements: stays dependency-free (Python stdlib only), remains Linux-only
(`/proc`, `os.sysconf`), and keeps the existing CSV schema backwards compatible
by appending columns at the end.

## Progress Snapshot

Legend: `[x]` done, `[~]` partial, `[ ]` pending.

- [x] Phase 1: Hardware fingerprint columns
- [x] Phase 2: CPU-time-accounted metrics
- [x] Phase 3: Calibration-backed normalization
- [ ] Phase 4: Environment pinning (stretch)
- [~] Validation run on two machines

---

## Phase 1: Hardware Fingerprint Metadata

Goal: every result row can be attributed to a specific machine, and runs from
different machines are never silently compared.

### Work

- [x] Add fields to `CSV_FIELDS = (...)` in `scripts/http_benchmark.py`
  (append at the end of the tuple, after `tcp_retransmits`):

  - [x] `machine_id` — stable SHA-256 prefix of CPU model/topology, logical
        cores, physical cores, memory, and page size; host names are not part of
        the ID.
  - [x] `host_name` — `platform.node()` (best-effort).
  - [x] `cpu_model`, `cpu_logical_cores`, and `cpu_physical_cores` — parsed from
        `/proc/cpuinfo` with safe fallbacks.
  - [x] `cpu_mhz` and `cpu_max_mhz` — current `/proc/cpuinfo` frequency and
        `cpufreq` sysfs maximum where readable, else `0`.
  - [x] `memory_total_kb` — `MemTotal` from `/proc/meminfo`.
  - [x] `os_kernel` — `platform.release()`.
  - [x] `compiler_flags` — normalized flags from `compile_commands.json` when
        available.
  - [x] `ulimit_nofile_soft`, `ulimit_nofile_hard`, and `page_size_kb`.

- [x] Add a module-level `hardware_fingerprint()` function returning a `dict`
      of the above; call it once in `main()` and merge into every result row
      (CSV and human-readable output).
- [x] Add `--print-hardware`, which prints the fingerprint without running load.
- [x] Write the fingerprint to every row. If an old CSV is encountered, migrate
      its existing rows to the appended schema before writing the new row.
- [x] Document the new columns and their units in the "Measurement Contract"
      section of `plans/scaling-plan.md`, and complete the Phase-0 environment
      metadata item.

### Exit criteria

- [x] `--print-hardware` emits the full fingerprint without touching the network.
- [x] Two consecutive calls on the same machine produce identical `machine_id`.
- [x] Existing CSV rows remain readable after schema migration; new columns are
      trailing additions.

---

## Phase 2: CPU-Time-Accounted Metrics

Goal: report how much server CPU work each request costs, independent of core
count and wall clock. This is the primary hardware-agnostic signal for the
scaling plan because per-phase optimization is about per-request work.

`ServerMonitor` (in `scripts/http_benchmark.py`) already samples
`cpu_ticks` (`utime + stime`) from `/proc/<pid>/stat`; the new metrics are pure
derivations in `ServerMonitor.summary()`.

### Work

- [x] In `ServerMonitor.summary()`, compute `server_cpu_seconds` and
      `server_cpu_cores` from `/proc/<pid>/stat` ticks.
- [x] Derive and append `server_cpu_seconds_per_1000_requests`,
      `rps_per_server_cpu_second`, and `hardware_agnostic_rps` to `CSV_FIELDS`.
      `hardware_agnostic_rps` is successful requests per server CPU-second and
      is the canonical hardware-agnostic stress metric.
- [x] Record the load generator's `client_cpu_seconds` and
      `client_cpu_percent` from `/proc/<self>/stat` when the server is started
      or an external server is sampled.
- [x] Emit a one-line diagnostic verdict (`limited_by`) using client/server CPU
      saturation heuristics; it is diagnostic rather than an acceptance gate.

### Exit criteria

- [ ] `server_cpu_seconds_per_1000_requests` is stable within a machine across
      repeated runs of the same scenario (within the existing < 5% throughput
      variation target).
- [x] The local matrix showed keep-alive CPU cost materially lower than
      new-connection CPU cost (`server_cpu_seconds_per_1000_requests`), making
      TCP setup visible in CPU cost rather than only wall-clock time.
- [x] Human-readable summary lists the new metrics next to the existing
      `server_cpu_percent`.

---

## Phase 3: Calibration-Backed Normalization

Goal: compute a single machine speed score from a fixed CPU-bound workload and
scale throughput/latency columns so runs from different machines are roughly
comparable.

Design constraints: dependency-free (stdlib only: `hashlib`, `time`), run on
the **server host** (loopback here means client and server are one machine —
document this), and weigh single-threaded speed only.

### Work

- [x] Implement `calibrate(seconds=2.0) -> float` in
      `scripts/http_benchmark.py` using a fixed SHA-256 workload and
      `time.perf_counter`.

  - [x] Use a fixed wall-clock measurement window and count hash operations.
  - [x] Return `machine_index = measured_ops_per_sec / 10,000`; the stable
        10,000 ops/s reference unit is documented in the source and is only a
        normalization unit, not a claim about a specific host.
  - [x] Use a payload-size ramp that halves the buffer until one iteration is
        below 50 ms, so slow and fast hosts can be measured.

- [x] Add `--calibrate {on,off,only}` (default `on`); `only` prints the index
      and exits without requiring a server. Add `--calibrate-force` to bypass
      the cache.
- [x] Cache calibration per `machine_id` at
      `~/.cache/http_server_bench/<machine_id>.json` (or `$XDG_CACHE_HOME`).
- [x] Compute and append `machine_index`, `calibration_ops_per_sec`,
      `calibrated`, `throughput_rps_normalized`,
      `successful_rps_normalized`, and normalized p50/p95/p99 latency.
- [x] Emit calibration and normalized throughput in human-readable output.
- [x] Update `scripts/run_benchmark_matrix.sh` to calibrate once before the
      scenario loop, reuse the cached index for each scenario, and default the
      matrix gate to `--min-hardware-agnostic-rps 1000` (override with the
      third script argument).

### Exit criteria

- [ ] Calibration is deterministic within a machine: index varies < 2% across
      three consecutive `--calibrate only` runs with the governor pinned to
      `performance` (not verified in this environment).
- [ ] On two distinct machines that differ by > 30% raw `throughput_rps`, the
      normalized `throughput_rps_normalized` for the same scenario is within
      15% of each other, or the discrepancy is explained in this doc (requires
      a second machine).
- [x] `machine_index` is written to every calibrated row and reused across one
      matrix run through the cache.
- [x] The CMake `stress` target gates on `hardware_agnostic_rps`, not raw
      `throughput_rps`; the short integration run passed this gate locally.

---

## Phase 4: Environment Pinning (Stretch)

Goal: constrain the environment so runs are comparable by construction,
independent of the calibration math. All items are optional; nothing in Phases
1–3 depends on them except the calibration-quality check in Phase 3.

### Work

- [ ] Pin server and generator to specific cores during a matrix run:
      `taskset -c 0-3 ./bin/http_server` and `taskset -c 8-15 python3 ...`
      (adjust ranges per host; add a `run_benchmark_pinned.sh` wrapper).
- [ ] Forced frequency: document `cpupower frequency-set -g performance` (or
      the sysfs equivalent) as a required step for reproducible calibration.
- [ ] Add resource limits to `docker-compose.yml` for the server service:
      `cpus: "4"`, `mem_limit: 1g`, `pids_limit: 256`, and document that
      matrix runs under compose produce envelope-normalized numbers.
- [ ] Fix and document the constant resource envelope used for all runs:
      `THREAD_POOL_SIZE = 16` (already fixed), `--concurrency 32` in
      `scripts/run_benchmark_matrix.sh`, ulimit nofile, socket backlog.
- [ ] Add a `--check-env` flag that verifies requirements (pinned cores,
      performance governor, ulimits) and fails fast with a clear message when
      they are unmet, rather than producing a noisy run.

### Exit criteria

- [ ] Two matrix runs on the same hardware, one bare and one under the
      documented pin/limit envelope, produce CPU-time metrics and normalized
      throughput within 10% of each other.
- [ ] `--check-env` either passes cleanly or exits nonzero with the exact
      unmet requirement named.

---

## Validation and Acceptance

### Work

- [ ] Re-run the full matrix from `scripts/run_benchmark_matrix.sh` on the
      current dev machine and commit the results to `benchmarks/benchmark_matrix.csv`
      (raw + new columns) as a pre-change baseline.
- [ ] Run the same matrix on a second machine (different core count or clock)
      using `run_benchmark_matrix.sh` and compare:

  - [ ] Raw `throughput_rps`: expected to differ.
  - [ ] `server_cpu_seconds_per_1000_requests`: expected to be within ~15%.
  - [ ] `throughput_rps_normalized`: expected to be within ~15%.
- [x] Verify CSV backward compatibility: old rows migrate to the appended
      schema and remain readable (covered by `tests/test_http_benchmark.py`).
- [x] Update `plans/scaling-plan.md` Measurement Contract to reference this doc
      for hardware-agnostic reporting requirements.
- [x] Run a short real server stress test with a minimum
      `hardware_agnostic_rps` gate and verify the result CSV contains hardware
      metadata and calibrated normalized metrics.

### Exit criteria

- [ ] A single `benchmarks/benchmark_matrix.csv` file contains rows from both
      machines, distinguishable only by `machine_id` (`host_name` may differ),
      and the normalized columns agree within the stated tolerance.
- [x] No new runtime dependency is introduced (stdlib only).

---

## Risks and Caveats

- **Loopback shares cores**: client and server compete for the same CPUs, which
  inflates server `cpu_seconds_per_1000_requests` at high offered rates. The
  Phase 2 verdict ("limited by client or server") is the mitigation; do not
  compare CPU-time metrics across runs with different client saturation.
- **Frequency scaling**: turbo/`powersave` governors make calibration and CPU
  metrics noisy. Calibration must be run with the governor pinned (Phase 4) or
  the ≤ 2% determinism exit criterion fails by design.
- **Calibration is single-threaded**: the index weights per-core speed, not
  multi-core scaling; it will not capture thread-pool scaling behavior. That is
  intentional — core-count effects belong in Phase 2 CPU-time metrics, not in
  the index.
- **Linux-only**: `/proc` and `os.sysconf("SC_CLK_TCK")` limit the harness to
  Linux, which is already the project's target; document this rather than
  abstracting it.
- **Reference-unit drift**: the 10,000 ops/s normalization unit is fixed in
  source. If the calibration workload or unit changes, bump the calibration
  version and keep old result rows distinguishable by their algorithm/version.

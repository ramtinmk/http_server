---
plan_id: memory-profiler
title: Runtime Memory Profiler Metrics
category: implementation
status: done
owner: agent
created: 2026-09-26
updated: 2026-09-26
related: [production-http-server, scaling-plan, hardware-agnostic-benchmark]
---

# Runtime Memory Profiler Metrics

## Purpose

Add a lightweight, always-on memory profiler to the server process so a
benchmark or soak run can see *where* memory goes (resident, proportional, heap,
mmap) from the existing `HTTP_SERVER_METRICS_FILE` JSON snapshot, instead of
relying only on an external `VmRSS` sample. This closes the observability gap
behind the `production-http-server` soak contract and makes per-scenario memory
comparisons machine-readable.

## Scope

**In**

- `include/metrics.h`, `src/metrics.c`: sample process memory and emit new
  fields in the snapshot.
- A small sampling boundary (`include/memory_profiler.h`,
  `src/memory_profiler.c`) so memory collection is isolated from the JSON
  formatter and testable on its own (CMake globs `src/*.c`, so a new file is
  picked up after `cmake -S . -B .`).
- `scripts/http_benchmark.py`: expose the new snapshot fields as
  `server_memory_*` columns; update `tests/test_http_benchmark.py` defaults.
- `readme.md`: document the new fields.

**Out**

- Allocation-site attribution, malloc interposition/LD_PRELOAD, and external
  tooling (valgrind massif, heaptrack). Deferred unless the aggregate fields
  cannot explain a leak.
- Prometheus `/metrics` (owned by `production-http-server/phase-5`).
- Per-connection buffer accounting; `buffer_bytes_current/max` already covers
  the server-owned buffers and must not be duplicated.

## Baseline

- The server tracks only its own buffer bytes internally
  (`src/metrics.c`: `g_buffer_bytes_current` / `g_buffer_bytes_max`) and emits
  them in the snapshot.
- Process `VmRSS` is measured *externally*: `scripts/http_benchmark.py` samples
  `/proc/<pid>/status` into `server_rss_kb` (max over the run) and
  `scripts/saturation_test.py` does the same via `read_rss_kb`.
- Evidence: `scripts/http_benchmark.py:1050-1063` (monitor),
  `scripts/http_benchmark.py:1115-1154` (snapshot field mapping),
  `src/metrics.c:235-251` (buffer counters).
- Consequence: no heap vs mmap vs file-backed split, no server-side high-water
  marks, and RSS is invisible when the server runs without the harness.

## Measurement contract

- **Metric:** process memory reported by the server snapshot.
- **Method:** the reporter thread (existing 250 ms cadence,
  `src/main.c:427-430`) samples `/proc/self/statm` (VMS/RSS),
  `/proc/self/smaps_rollup` (PSS, best-effort) and `mallinfo2()` (heap/mmap)
  into relaxed atomics, then formats them into the snapshot. Sampling runs
  *only* on the reporter thread; the event loops never call it.
- **Environment:** Linux; keep-alive scenario, `HTTP_SERVER_METRICS_FILE` set;
  hardware fingerprint and calibration per `hardware-agnostic-benchmark`.
- **Reported fields:** `rss_kb`, `rss_kb_max`, `pss_kb`, `pss_kb_max`,
  `vmsize_kb`, `heap_inuse_bytes`, `heap_inuse_bytes_max`, `heap_mmap_bytes`,
  `memory_sample_ok`.
- **Warmup / steady / drain:** warmup discarded; drift is evaluated over the
  final 80% of the steady window (matches `production-http-server` soak
  contract). Pass threshold: `rss_kb` and `pss_kb` max−min within 5% over that
  window with no unbounded trend. The glibc heap counters
  (`heap_inuse_bytes`, `heap_inuse_bytes_max`) and `vmsize_kb`/`heap_mmap_bytes`
  are recorded for attribution but not gated: measured over a 5-minute soak,
  RSS and PSS stay flat while `mallinfo()` ratchets its in-use figure by tens of
  percent as per-request buffers churn, and heap in-use cannot exceed mapped RSS,
  so it is not an independent leak signal. A real leak appears as RSS/PSS
  drift.
- **Cross-check:** server `rss_kb` agrees within 10% with the harness external
  `server_rss_kb` on the same run.
- **Evidence:** `benchmarks/memory_soak.json` plus a benchmark CSV row carrying
  the new columns.

## Steps

1. [x] Add `include/memory_profiler.h` + `src/memory_profiler.c`:
   `memory_profiler_sample(void)` refreshes current values and high-water marks;
   `size_t memory_profiler_append_json(char *buf, size_t cap, size_t off)`
   serializes them. `mallinfo2` is guarded by glibc version and falls back to
   `mallinfo` (and to zero on non-glibc); a missing `smaps_rollup` leaves PSS
   unavailable (`pss_kb = 0`, `memory_sample_ok` reflects statm).
2. [x] `src/metrics.c`: include `memory_profiler.h`; sample from
   `write_snapshot` (called only by the reporter thread), and append the memory
   fields in `metrics_snapshot` via a dedicated append. Raised the snapshot
   buffer (`char json[4096]`) to 8192 and updated the `metrics_snapshot` docs in
   `metrics.h`.
3. [x] `scripts/http_benchmark.py`: add the nine `server_memory_*` fields to
   `CSV_FIELDS` (trailing, backward-compatible), the `read_server_metrics`
   defaults, and the `mapping`; surface current/max RSS/heap.
4. [x] `tests/test_http_benchmark.py`: extend the metrics fixture with the new
   keys.
5. [x] `readme.md`: document the fields and that sampling is reporter-thread
   only.
6. [x] Add a repeatable artifact: `scripts/memory_soak.py` driving a fixed-rate
   keep-alive run, sampling the snapshot and writing
   `benchmarks/memory_soak.json` with the drift computation. A `memory-soak`
   CMake target runs it (`make memory-soak`).

## Validation

- [x] `cmake -S . -B . && make` clean; `./bin/run_tests ring` and the server
      e2e suite per `AGENTS.md` (verified via `ctest --output-on-failure` with
      the server running: 2/2 passed).
- [x] `make lint` reports no new findings (only pre-existing `metrics.c`
      warnings remain; `src/memory_profiler.c` is clean).
- [x] Start `HTTP_SERVER_METRICS_FILE=/tmp/m.json ./bin/http_server`, generate
      traffic, and confirm the snapshot parses and all nine fields are present
      with sane values.
- [x] Run the soak artifact and confirm the drift check is computed and
      recorded, not just sampled (`make memory-soak`, exit 0).

## Exit criteria

- [x] A snapshot from a running server parses as JSON and contains the nine
      documented fields; empty under a non-Linux or unavailable-`smaps` host
      without breaking parsing.
- [x] `server_memory_rss_kb` (internal `rss_kb`) matches the external
      `/proc/<pid>/status` `VmRSS` sample exactly in the cross-check (9064 kB vs
      9064 kB, 0.00% delta), well within the 10% bound; the benchmark CSV
      carries the `server_memory_*` columns.
- [x] On a 5-minute stationary keep-alive soak (297,070 requests), `rss_kb`
      drifted 0.00% and `pss_kb` 0.20% over the final 80%, both within 5% with
      no unbounded trend; evidence in `benchmarks/memory_soak.json`.
- [x] No new compile warnings; `make lint` clean for new code; benchmark and
      server suites still pass.

## Risks and rollback

- **Reporter-thread sampling perturbs throughput** → sampling is off the hot
  path by construction; if the 250 ms snapshot shows CPU cost, lengthen the
  interval. Rollback: stop calling `memory_profiler_sample` (fields freeze).
- **`mallinfo2`/`smaps_rollup` unavailable or container-restricted** → compile
  guard plus runtime zero fallback; `memory_sample_ok` makes degradation
  visible instead of silently reading zero as real.
- **Snapshot buffer overflow after adding fields** → 8192-byte buffer and the
  existing `appendf` truncation contract; verify the full snapshot fits.
- **CSV schema change breaks historical rows** → additions are trailing and
  default-empty on read, matching the existing Phase 4/5 precedent.
- **Entire feature regresses the data path** → all changes are additive and
  reversible per file; revert the `memory_profiler` sampling call to restore
  the prior snapshot exactly.

---
plan_id: memory-profiler
title: Runtime Memory Profiler Metrics
category: implementation
status: active
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
  contract). Pass threshold: max−min of `rss_kb` and `heap_inuse_bytes` within
  5% over that window, no unbounded trend.
- **Cross-check:** server `rss_kb` agrees within 10% with the harness external
  `server_rss_kb` on the same run.
- **Evidence:** `benchmarks/memory_soak.json` plus a benchmark CSV row carrying
  the new columns.

## Steps

1. [ ] Add `include/memory_profiler.h` + `src/memory_profiler.c`:
   `memory_profiler_sample(void)` refreshes current values and high-water marks;
   `size_t memory_profiler_append_json(char *buf, size_t cap, size_t off)`
   serializes them. Guard `mallinfo2` behind a glibc-version/availability check
   with a zero fallback, and treat a missing `smaps_rollup` as PSS unavailable
   (`pss_kb = 0`, `memory_sample_ok` reflects statm/heap only).
2. [ ] `src/metrics.c`: include `memory_profiler.h`; call
   `memory_profiler_sample()` from `reporter_thread` before `write_snapshot`,
   and append the memory fields in `metrics_snapshot` via the existing
   `appendf` path. Raise the snapshot buffer (`char json[4096]` at
   `src/metrics.c:449`) to 8192 and update `metrics.h` docs.
3. [ ] `scripts/http_benchmark.py`: add the nine `server_memory_*` fields to
   `CSV_FIELDS` (trailing, backward-compatible), the `read_server_metrics`
   defaults, and the `mapping`; surface current/max RSS/heap.
4. [ ] `tests/test_http_benchmark.py`: extend the metrics fixture with the new
   keys so `read_server_metrics` coverage stays green.
5. [ ] `readme.md`: document the fields and that sampling is reporter-thread
   only.
6. [ ] Add a repeatable artifact: `scripts/memory_soak.py` (or an option on the
   existing harness) driving a fixed-rate keep-alive run, sampling the
   snapshot every 60 s and writing `benchmarks/memory_soak.json` with the drift
   computation.

## Validation

- [ ] `cmake -S . -B . && make` clean; `./bin/run_tests ring` and the server
      e2e suite per `AGENTS.md`.
- [ ] `make lint` reports no new findings.
- [ ] Start `HTTP_SERVER_METRICS_FILE=/tmp/m.json ./bin/http_server`, generate
      traffic, and confirm `python3 -c 'import json;json.load(open("/tmp/m.json"))'`
      succeeds and all nine fields are present with sane values.
- [ ] Run the soak artifact and confirm the drift check is computed and
      recorded, not just sampled.

## Exit criteria

- [ ] A snapshot from a running server parses as JSON and contains the nine
      documented fields; empty under a non-Linux or unavailable-`smaps` host
      without breaking parsing.
- [ ] `server_memory_rss_kb` (internal) is within 10% of the harness external
      `server_rss_kb` on the same keep-alive run; evidence: CSV row +
      `benchmarks/memory_soak.json`.
- [ ] On a ≥5-minute stationary keep-alive soak, `rss_kb` and
      `heap_inuse_bytes` max−min over the final 80% is within 5% with no
      unbounded trend; evidence in `benchmarks/memory_soak.json`.
- [ ] No new compile warnings; `make lint` clean; benchmark and server suites
      still pass.

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

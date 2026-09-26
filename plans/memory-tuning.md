---
plan_id: memory-tuning
title: Memory Tuning and Profiler Refinements
category: implementation
status: done
owner: agent
created: 2026-09-26
updated: 2026-09-26
related: [memory-profiler]
---

# Memory Tuning and Profiler Refinements

## Purpose

Act on the memory analysis that followed the `memory-profiler` plan: bound the
glibc per-thread arena reservation that makes VmSize scale with the event-loop
count (up to ~586 MB at 8 loops) and make the soak artifact trustworthy by
removing the pre-initialization sample, summarizing the VMS/mmap series, and
adding a stabler heap proxy (`Private_Dirty`) than ratcheting `mallinfo()`.

## Scope

**In**

- `include/server_config.h`, `src/main.c`: cap glibc arenas with
  `mallopt(M_ARENA_MAX, ...)` before any thread starts, overridable by
  `HTTP_SERVER_MALLOC_ARENA_MAX` and deferring to a pre-set `MALLOC_ARENA_MAX`.
- `include/memory_profiler.h`, `src/memory_profiler.c`, `include/metrics.h`:
  add `private_dirty_kb`, read from the same `smaps_rollup` pass as PSS.
- `scripts/http_benchmark.py`, `tests/test_http_benchmark.py`: expose the new
  field as a trailing `server_memory_*` column.
- `scripts/memory_soak.py`: discard pre-initialization samples via a warmup and
  summarize `vmsize_kb`, `heap_mmap_bytes`, and `private_dirty_kb` in the drift
  block (recorded, not gated).
- `readme.md`: document the arena cap, the warmup, and the new field.
- `benchmarks/memory_soak.json`: regenerate with the refined artifact.

**Out**

- Shrinking `ELConnection` / lazily allocating the pipeline queue (needs a
  hot-path spike, tracked as a separate future plan).
- Per-loop input-buffer free lists (perf follow-up, not required to fix the
  reported memory symptom).

## Baseline

- 8 loops, 70 s at 1000 req/s: VMS = 599,876 kB, RSS = 8,996 kB; the same run
  with `MALLOC_ARENA_MAX=1` gives VMS = 75,592 kB, RSS = 9,064 kB (measured
  2026-09-26 on the 8-core host, `env MALLOC_ARENA_MAX=N ./bin/http_server` +
  `scripts/memory_soak.py`).
- `MALLOC_ARENA_MAX=2`: VMS = 141,124 kB, RSS = 9,064 kB.
- Idle RSS is capacity-driven, not loop-driven: 2,060 kB at
  `HTTP_SERVER_MAX_CONNECTIONS=64` vs 9,048 kB at 1024, independent of loop
  count.
- `benchmarks/memory_soak.json` records a t=0 sample of RSS 572 kB /
  VMS 76,548 kB captured before `event_loop_run` builds the per-loop tables and
  are not representative of the steady run (9,024 kB / 599,876 kB).
- `heap_mmap_bytes` is sampled per interval but absent from the soak's `drift`
  summary; `mallinfo().uordblks` ratchets (50,960 → 107,072) while RSS is flat,
  so it is not used as a leak gate.

## Measurement contract

- **Metric:** process VMS and RSS from the server snapshot (`vmsize_kb`,
  `rss_kb`) and the soak artifact's drift block.
- **Method:** `scripts/memory_soak.py` against `./bin/http_server` at
  1000 req/s / 16 keep-alive connections, 8 loops, snapshot every 100 ms; and a
  direct `VmSize` read from `/proc/<pid>/status` under the same load.
- **Environment:** 8-core Linux host, glibc 2.31; same hardware as the
  `memory-profiler` baseline.
- **Pass threshold:** default (no env override) 8-loop VMS ≤ 200,000 kB;
  RSS within 10% of the 9,000 kB baseline; the regenerated soak artifact's
  first recorded sample is ≥ 5,000 kB RSS and the `drift` block contains
  `vmsize_kb`, `heap_mmap_bytes`, and `private_dirty_kb`.
- **Reported fields:** snapshot `rss_kb`, `vmsize_kb`, `heap_mmap_bytes`,
  `private_dirty_kb`; soak `samples[]` and `drift`.

## Steps

1. [x] `include/server_config.h`: add `MALLOC_ARENA_MAX_DEFAULT` (2) and
   `ENV_MALLOC_ARENA_MAX` with a comment explaining the VMS-vs-contention
   tradeoff (1 minimizes VMS, 2 keeps one secondary arena).
2. [x] `src/main.c`: add `configure_allocator()` (glibc-guarded) called before
   `metrics_reporter_start` so every thread inherits the cap; respect a
   pre-set `MALLOC_ARENA_MAX`, parse `HTTP_SERVER_MALLOC_ARENA_MAX`, fail
   clearly on a malformed value, and print the applied cap.
3. [x] `src/memory_profiler.c` / `include/memory_profiler.h`: parse
   `Private_Dirty` in the existing `smaps_rollup` pass and emit
   `private_dirty_kb`.
4. [x] `include/metrics.h`: extend the snapshot field list in the docs.
5. [x] `scripts/http_benchmark.py`: add `server_memory_private_dirty_kb` as a
   trailing column (defaults + mapping).
6. [x] `tests/test_http_benchmark.py`: add the new key to the metrics fixture.
7. [x] `scripts/memory_soak.py`: add `--warmup` (default 1.0 s) so sampling
   starts after the event loops exist, record it in `config`, and add
   `vmsize_kb`, `heap_mmap_bytes`, `private_dirty_kb` to `DRIFT_CHECKS` as
   non-gated series.
8. [x] `readme.md`: document the arena cap, the soak warmup, and
   `private_dirty_kb`.
9. [x] Regenerate `benchmarks/memory_soak.json` via `make memory-soak`.

## Validation

- [x] `cmake -S . -B . && make` clean; `ctest --output-on-failure` with the
      server running (2/2) per `AGENTS.md`; `./bin/run_tests ring` passes.
- [x] `make lint` reports no new findings (only the pre-existing `main.c`
      and `metrics.c` warnings remain; touched files clean).
- [x] 8-loop load with no env override: snapshot `vmsize_kb` = 141,124 kB
      (≤ 200,000) and RSS = 9,072 kB (within 10% of 9,000); with
      `HTTP_SERVER_MALLOC_ARENA_MAX=1` VMS = 75,852 kB (≤ 100,000).
- [x] `HTTP_SERVER_MALLOC_ARENA_MAX=abc ./bin/http_server` exits 1 with
      `FATAL: HTTP_SERVER_MALLOC_ARENA_MAX=abc is not a positive integer`; a
      pre-set `MALLOC_ARENA_MAX=4` prints `preset by MALLOC_ARENA_MAX=4`.
- [x] Regenerated `benchmarks/memory_soak.json` has a first sample at t=1.0
      (RSS 9,072 kB, not the pre-init 572 kB) and a `drift` block containing
      the new series.

## Exit criteria

- [x] Default 8-loop VMS under the measured load ≤ 200,000 kB (baseline
      599,876 kB): measured 141,124 kB across the 5-minute soak, RSS flat at
      9,072 kB (0.00% drift); evidence: `benchmarks/memory_soak.json`.
- [x] `benchmarks/memory_soak.json` records post-warmup samples (t=1…241),
      lists `private_dirty_kb` per sample, and summarizes `vmsize_kb`,
      `heap_mmap_bytes`, and `private_dirty_kb` in `drift`.
- [x] Snapshot JSON still parses and all previous fields are unchanged;
      `private_dirty_kb` = 7,508 kB and `server_memory_private_dirty_kb` is
      wired into `CSV_FIELDS`/defaults/mapping.
- [x] `ctest` (2/2) and `make lint` (exit 0) pass as above.

## Risks and rollback

- **Arena cap serializes malloc on the hot path** → default is 2 (one
  secondary arena); measure throughput before/after; an operator can set
  `HTTP_SERVER_MALLOC_ARENA_MAX` or a pre-set `MALLOC_ARENA_MAX`. Rollback:
  remove the `configure_allocator()` call (VMS returns to previous behavior).
- **`mallopt` unavailable outside glibc** → compile guard plus a printed
  "not applicable" line; no behavior change on other libcs.
- **`Private_Dirty` missing on restricted procfs/older kernels** → left at 0
  like PSS; `memory_sample_ok` already covers statm availability.
- **New CSV column shifts historical rows** → appended last with a default,
  matching the existing trailing-additions precedent.
- **Warmup hides a real early leak** → warmup is one second against a
  250 ms reporter cadence and is recorded in `config`; full snapshots remain
  available via `HTTP_SERVER_METRICS_FILE` for any run that needs them.
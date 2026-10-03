---
plan_id: bare-metal-performance
title: Bare-Metal Performance Program
category: program
status: draft
owner: agent
created: 2026-10-03
updated: 2026-10-03
related: [production-http-server, scaling-plan, scaling-phase-4-scale-accept, memory-tuning, hardware-agnostic-benchmark]
---

# Bare-Metal Performance Program

## Purpose

Every performance number this project has published so far was measured under
WSL2, where syscalls are intercepted and scheduling is virtualized. That
distorts exactly the paths a static file server spends its time in — body
writes, `sendfile`, TLS record I/O — and it means the current "we are close to
nginx" story is not trustworthy on a real Linux host. This program re-establishes
the baseline on bare metal and then closes the one measured, named gap: cached
plaintext file serving, where nginx leads 1.3–2.1× because it `sendfile`s from
page cache while we `send()` from our own heap cache.

This is a `program` plan. It defines the phases, gates, and acceptance bar for
the effort; each phase gets its own `implementation` plan before work starts, per
`plan-spec.md`. It is written to execute **after** the `production-http-server`
program's remaining external validation (the multi-hour soak and the first remote
CI run) so the two do not contend for the host.

## Baseline

Recorded, host-specific, and WSL2-derived; the shape matters, the absolutes do
not carry over to bare metal.

- **Peer comparison** (`benchmarks/nginx_comparison.csv` / `.json`, 8-core
  `i5-1135G7`, WSL2, servers pinned 0-3, `wrk` 4-7, best of 2). Ratio is
  `nginx / ours`, so below 1.0 means we are faster:

  | mode | tiny | small | medium | binary | streamed | large |
  | --- | ---: | ---: | ---: | ---: | ---: | ---: |
  | plaintext identity | 1.33 | 1.27 | 1.47 | 2.11 | 1.09 | 0.84 |
  | gzip (ours cached vs `gzip_static`) | 1.48 | 1.25 | 1.25 | — | — | — |
  | TLS | 0.82 | 1.33 | 1.05 | 0.94 | 1.02 | 0.97 |

  Cause, already stated in `readme.md`: nginx `sendfile`s plaintext cached files
  from page cache; we `send()` from the heap cache. Under TLS neither can
  `sendfile`, the file-serving edge disappears, and we are at parity or ahead.
- **Capacity report** (`benchmarks/production_phase6_capacity.json`, `i9-13900K`,
  WSL2): plaintext `hardware_agnostic_rps` 14,753.974 (p99 0.632 ms); TLS
  9,801.047 (p99 1.29 ms); **TLS/plaintext ratio 0.6643**; the `2×` overload run
  is bounded (p99 4.99 ms, zero client timeouts, `listener_disabled_count=0`).
- **Peak snapshot** (`readme.md`, `i9-13900K`, WSL2): ~751k req/s on the tiny
  cached asset at 8 `wrk` threads; loopback/client-bound beyond that.
- **Memory:** the program default is `MALLOC_ARENA_MAX=2`; an 8-loop run holds
  VMS ~141,124 kB, RSS ~9,072 kB, flat (`plans/memory-tuning.md`).
- **Unclosed performance work:** `plans/scaling-plan-phase4.md` Step A (capture a
  profile, name the limiter, write a decision record) is entirely unchecked;
  idle-buffer decoupling (4.B.5) and saturation/buffer metrics remain open;
  `plans/memory-tuning.md` explicitly deferred per-loop input-buffer free lists.

## Goals and Non-goals

### Goals

- **Trustworthy baseline.** Re-measure the file-class matrix and the nginx
  comparison on a native Linux host, with a recorded host fingerprint, before
  changing any code.
- **Close the cached-plaintext gap.** Bring plaintext `medium` and `binary`
  within 1.15× of nginx (from 1.47× and 2.11×) without losing the TLS parity/win.
- **Raise TLS efficiency.** TLS/plaintext `hardware_agnostic_rps` ratio ≥ 0.80
  (from 0.66) with zero handshake or keep-alive failures.
- **Cut per-request cost where a profile proves it.** Fewer syscalls and less
  copy/allocator work per response, evidenced by `perf`/`strace`, not intuition.
- **No regression** of the `scaling-plan` and `production-http-server` gates
  (5,000 req/s acceptance, zero unexpected errors, bounded overload, traversal
  corpus, flat RSS/FD).

### Non-goals

- HTTP/2, HTTP/3, or any protocol change (a separate future program; see
  `production-http-server` non-goals).
- Dynamic content, compression-on-the-fly, reverse proxy, or caching layers.
- Beating nginx on every class. `streamed`/`large` are bandwidth- or client-bound
  and are explicitly out of the acceptance bar.
- Kernel/OS tuning beyond what is already documented in `scaling-phase-5`
  (`somaxconn`, `tcp_*`, governor); host configuration is a measurement
  prerequisite here, not a deliverable.
- Rewriting the event loop or replacing the memory cache wholesale; changes must
  be independently reversible.
- Micro-optimizations with no profile evidence and no named metric.

## Measurement contract

Reuse the `scaling-plan` / `hardware-agnostic-benchmark` contract (warmup,
steady, drain; fixed-rate and fixed-duration; host fingerprint; calibration;
`hardware_agnostic_rps`) and the `docs/benchmarks.md` per-class reporting rules.
This program adds:

- **Bare-metal host fingerprint.** Record CPU model, physical/logical cores,
  kernel, governor, `somaxconn`, `RLIMIT_NOFILE`, `tcp_rmem`/`tcp_wmem`, NUMA
  topology, and filesystem/page-cache state via the existing
  `--check-env`/fingerprint fields. A run without a fingerprint is invalid.
- **Pinned, disjoint CPU sets.** Server and generator on disjoint CPUs via the
  pinned runner; the server's loop count fixed and recorded. Never benchmark
  while the soak, a CI job, or another harness runs.
- **Per-class result grid.** One row per `class × size × gzip × keep-alive ×
  TLS × concurrency`; report RPS and p99 for `tiny`/`small`/`medium`/`binary`,
  MB/s for `streamed`/`large`, and the `limited_by` column. No single-average
  claims.
- **Peer ratio.** Report `nginx / ours` per class from the deterministic corpus,
  both servers built `-O2` with matched worker counts and identical asset bytes
  (manifest SHA-256), for identity, gzip-precompressed, and TLS modes.
- **Syscall/CPU profile.** `perf stat -d`, `perf record` flame data, and
  `strace -c` per class before and after each change; report syscalls/request and
  CPU-seconds/1000 requests.
- **Pass thresholds.** As in Goals: plaintext `medium`/`binary` ratio ≤ 1.15,
  TLS/plaintext ratio ≥ 0.80, `scaling-plan` acceptance held, zero unexpected
  errors, no RSS/FD growth. Evidence is the regenerated comparison CSV/JSON, the
  capacity report, the profile artifacts, and the test suites.

## Progress snapshot

Legend: `[x]` done, `[~]` partial, `[ ]` pending, `[!]` blocked.

- [ ] Phase 0 `bare-metal-performance/phase-0`: bare-metal baseline and
  profiling decision gate.
- [ ] Phase 1 `bare-metal-performance/phase-1`: cached plaintext file path.
- [ ] Phase 2 `bare-metal-performance/phase-2`: per-request syscall, buffer, and
  memory reduction.
- [ ] Phase 3 `bare-metal-performance/phase-3`: TLS record path.
- [ ] Phase 4 `bare-metal-performance/phase-4`: bare-metal peer re-comparison and
  release gate.

## Phases

### bare-metal-performance/phase-0: Bare-metal baseline and profiling gate

- **Objective:** Establish a native-Linux baseline and name the limiter before
  any optimization, so every later change cites profile evidence.
- **Complexity:** 3 — one new measurement environment plus analysis; no code.
- **Risk:** medium — a misconfigured host or a noisy measurement invalidates
  every downstream comparison.

**Work**

- [ ] Provision or identify a native Linux measurement host; record the host
  fingerprint, governor, CPU pinning, and page-cache warm state per run.
- [ ] Re-run the deterministic corpus and the nginx comparison on bare metal for
  identity, gzip-precompressed, and TLS modes; publish a new CSV/JSON and update
  the `readme.md` table only after the run.
- [ ] Re-run `make phase6-capacity` on bare metal to re-baseline plaintext/TLS
  `hardware_agnostic_rps` and the TLS/plaintext ratio.
- [ ] Capture `perf stat -d`, `perf record`, and `strace -c` for plaintext
  `medium`/`binary` and for TLS `tiny`/`medium`; attribute time and syscalls to
  `accept`, `epoll_wait`, parse, `send`/`writev`/`sendfile`, and TLS record I/O.
- [ ] Record the pre-change counters named by `scaling-plan-phase4` Step A
  (`el_*`, CPU-seconds/1000 requests, active high-water mark).
- [ ] Write a decision record that names the limiter for each target and selects
  the direction of Phase 1/3; do not proceed to Phase 1 without it.

**Exit criteria**

- [ ] A bare-metal host fingerprint and a regenerated per-class comparison
  exist; evidence: the comparison CSV/JSON and its `notes`.
- [ ] A decision record names the plaintext cached-file limiter and the TLS
  limiter with `perf`/`strace` evidence; evidence: the profile artifacts.
- [ ] `plans/scaling-plan-phase4.md` Step A is closed against the recorded
  evidence (or its open items are explicitly re-scoped into this plan).

### bare-metal-performance/phase-1: Cached plaintext file path

- **Objective:** Serve plaintext file-backed responses without copying cached
  bytes through the heap, matching nginx on `medium`/`binary` while preserving
  the TLS/gzip/tiny behavior.
- **Complexity:** 4 — cross-module: response generation, the file cache, and the
  nonblocking write path, with interacting invariants (partial writes, ranges,
  `HEAD`, validators). *Not split:* it is one response path proven by one
  acceptance ratio and one regression gate; splitting would add coordination
  cost without reducing uncertainty. Justification recorded per `plan-spec.md`
  §7.
- **Risk:** high — a partial-write or offset bug corrupts responses or leaks
  descriptors; the write path is the hottest and most concurrent code.

**Work**

- [ ] Choose the mechanism the Phase 0 profile supports: page-cache-backed
  `sendfile` for plaintext file-backed bodies, or `writev` header+body coalescing
  where `sendfile` cannot apply; record the choice and rejected alternative.
- [ ] Keep the in-memory cache for TLS, gzip representations, `tiny`, and any
  body that is not a simple file-backed identity response so the TLS parity/win
  is not lost.
- [ ] Preserve per-connection offset across `EAGAIN`/partial progress; retain
  byte-range (`206`) and `HEAD` semantics; never hold a file descriptor across an
  idle keep-alive connection beyond the configured budget.
- [ ] Add metrics for the new path: `sendfile` calls, partial writes, and
  cache-vs-file hits, exported per the existing trailing-column rule.
- [ ] Prove equivalence with a cursor/offset test over ragged and slow-reader
  clients, plus the existing range and conditional suites.

**Exit criteria**

- [ ] Plaintext `medium` and `binary` `nginx / ours` ratio ≤ 1.15 on the
  bare-metal host, best of N with fingerprint; evidence: the comparison CSV.
- [ ] TLS `tiny`/`binary` remain at parity or ahead (ratio ≤ 1.0) and no TLS
  class regresses by more than 5% versus the Phase 0 baseline; evidence: the TLS
  comparison rows.
- [ ] Byte-range, `HEAD`, conditional, and slow-reader cases return byte-identical
  bodies to the pre-change server; evidence: the server E2E suite plus a recorded
  differential run.

### bare-metal-performance/phase-2: Per-request syscall, buffer, and memory reduction

- **Objective:** Reduce syscalls and allocation/copy work per response where the
  Phase 0 profile shows cost, including the deferred idle-buffer work.
- **Complexity:** 3 — several localized hot-path changes behind one profiling
  loop; incrementally testable.
- **Risk:** medium — hot-path changes can trade latency for throughput or
  reintroduce allocator contention.

**Work**

- [ ] Coalesce response header and body writes where the profile shows separate
  `send`/`writev` calls dominate.
- [ ] Decouple idle keep-alive state from request/response storage so memory
  scales with active requests rather than table size (closes
  `scaling-plan-phase4` 4.B.5).
- [ ] Add per-loop input-buffer free lists, the follow-up deferred by
  `plans/memory-tuning.md`, only if the profile shows allocator cost on the hot
  path.
- [ ] Re-check allocator settings (`MALLOC_ARENA_MAX`) against the bare-metal
  profile; keep the change reversible by environment variable.
- [ ] Record syscalls/request, CPU-seconds/1000 requests, RSS, and VMS before and
  after each accepted change; revert any change without a named-metric win.

**Exit criteria**

- [ ] Syscalls per request for `small`/`medium` are reduced by ≥ 20% from the
  Phase 0 measurement, with no p99 regression > 5%; evidence: `strace -c`
  before/after and the comparison CSV.
- [ ] Idle-connection storage scales with active requests, not the table size;
  evidence: RSS at idle vs the configured connection table, recorded.
- [ ] No unbounded RSS/VMS/FD growth under the saturation and mixed-path
  scenarios; evidence: the metrics snapshot and soak harness.

### bare-metal-performance/phase-3: TLS record path

- **Objective:** Reduce TLS handshake and record-I/O cost so the TLS/plaintext
  ratio reaches the target without weakening protocol policy.
- **Complexity:** 4 — the TLS record state machine interacts with the nonblocking
  loop and per-connection buffers. *Not split:* the library owns the
  cryptography; the bounded work is buffer sizing, batching, and session reuse,
  verified incrementally. Justification recorded per `plan-spec.md` §7.
- **Risk:** medium — misconfiguration weakens security; batching changes can
  stall a loop or drop connections.

**Work**

- [ ] Profile the TLS path (`perf`/`strace`, OpenSSL counters) to separate
  handshake cost, record read/write batching, and file-body `pread`+`SSL_write`
  cost.
- [ ] Tune TLS record buffering and `SSL_write` batching within the existing
  bounded buffer, preserving TLS 1.2+, ALPN `http/1.1`, and no-0-RTT policy.
- [ ] Measure session-resumption rate and handshake latency; improve reuse only
  through supported, policy-preserving mechanisms.
- [ ] Keep the plaintext listener as the reference path and fallback; the change
  stays behind the existing config toggle.
- [ ] Extend the TLS keep-alive churn E2E test to cover the tuned path at high
  concurrency.

**Exit criteria**

- [ ] TLS/plaintext `hardware_agnostic_rps` ratio ≥ 0.80 on the bare-metal host,
  with zero handshake failures and zero dropped keep-alive responses; evidence:
  the capacity report and the TLS artifact.
- [ ] A local protocol/cipher/ALPN scan still reports no weak protocol, cipher,
  or certificate finding; evidence: the Phase 3 TLS artifact.
- [ ] `./bin/run_tests tls` passes including the extended churn case.

### bare-metal-performance/phase-4: Bare-metal peer re-comparison and release gate

- **Objective:** Publish the final bare-metal comparison and prove no regression
  of the correctness and stability gates before the program closes.
- **Complexity:** 2 — measurement and documentation around tested code.
- **Risk:** low — no protocol or code change.

**Work**

- [ ] Re-run the full per-class matrix and the nginx comparison on bare metal;
  regenerate `benchmarks/nginx_comparison.*` and the capacity report.
- [ ] Update `readme.md` benchmarking tables, `docs/benchmarks.md`, and the
  comparison runbook with the bare-metal host and results.
- [ ] Re-run the `scaling-plan` acceptance scenarios, the saturation harness, and
  the multi-hour soak to confirm no memory/FD regression.
- [ ] Record the profile artifacts and the before/after syscall and CPU tables as
  committed evidence.

**Exit criteria**

- [ ] Published per-class comparison shows plaintext `medium`/`binary` ≤ 1.15×
  and TLS ratio ≥ 0.80 versus nginx on the same bare-metal host; evidence:
  `benchmarks/nginx_comparison.json` + the capacity report.
- [ ] `scaling-plan` 5,000 req/s acceptance, zero unexpected errors, and bounded
  above-capacity behavior still hold; evidence: the regenerated capacity report.
- [ ] Ring/server/TLS suites green and the soak shows RSS/PSS/FD drift within 5%;
  evidence: the test output and `benchmarks/production_phase6_soak.json`.
- [ ] Every claimed speedup names the metric changed and the profile that
  justified it; no change ships on intuition alone.

## Gates

These must remain green for any phase to merge or proceed:

- `production-http-server` and `scaling-plan` correctness/regression gates,
  including the overload contract (zero timeouts, bounded `503`), the graceful
  shutdown contract, and the traversal corpus.
- Build, lint, and the focused suites (`ring`, `tls`) and the server E2E suite
  per `AGENTS.md`; no new compile warnings.
- Fuzz smoke and ASan/UBSan clean; coverage at or above the checked-in baseline.
- No unbounded growth in RSS, VMS, FDs, active connections, or cache bytes.
- Every benchmark report carries a bare-metal host fingerprint and a recorded
  CPU pinning; a comparison without them is invalid and must not be published.
- No benchmark runs concurrently with the soak or another harness on the same
  host.

## Acceptance

Program completion requires all of:

- [ ] Bare-metal baseline and profiling decision record published.
- [ ] Plaintext `medium`/`binary` cached-file ratio ≤ 1.15× nginx, best of N,
  with a recorded fingerprint.
- [ ] TLS/plaintext `hardware_agnostic_rps` ratio ≥ 0.80 with zero handshake
  failures.
- [ ] `scaling-plan` 5,000 req/s acceptance held; bounded overload; zero
  unexpected errors.
- [ ] Per-request syscall and CPU reductions evidenced by before/after profiles.
- [ ] No regression in the ring/server/TLS suites and no RSS/PSS/FD drift
  regression in the soak.
- [ ] `readme.md`, `docs/benchmarks.md`, and the comparison runbook updated to the
  bare-metal results.

## Risks and rollback

- **`sendfile`/`writev` path corrupts responses or leaks descriptors** → keep the
  heap-`send` path selectable behind a config toggle; differential-test bodies
  and revert the toggle if any mismatch appears.
- **Cached-file change regresses TLS** → keep the in-memory cache for TLS/gzip/
  tiny; gate on the TLS comparison rows; disable the new path if any TLS class
  drops > 5%.
- **Hot-path change trades latency for throughput** → hold a per-class p99 gate
  and revert changes without a named-metric win.
- **Benchmark host drift or WSL2/native mixing invalidates comparisons** → reuse
  the fingerprint and calibration; treat cross-host or cross-mode comparisons as
  invalid and re-measure.
- **Measurement is client-bound and mistaken for a server result** → require the
  `limited_by` column and MB/s reporting for `streamed`/`large`; exclude
  client-bound rows from acceptance.
- **Scope creep toward HTTP/2 or dynamic content** → explicit non-goals; a new
  program plan is required to change them.
- **Allocator tuning reintroduces contention** → default stays `MALLOC_ARENA_MAX=2`
  behind the environment override; measure throughput before/after.

## Execution order

1. Phase 0 — bare-metal baseline and profiling gate (unblocks every later
   phase).
2. Phase 1 — cached plaintext file path (the primary gap).
3. Phase 2 — syscall/buffer/memory reduction (profile-justified only).
4. Phase 3 — TLS record path.
5. Phase 4 — bare-metal peer re-comparison, soak re-run, and release gate.

Phase 0 must complete before any code change. Phases 1 and 2 share the profiling
loop and may interleave; Phase 3 is independent of Phase 1 (different path) but
depends on the Phase 0 TLS profile. Run this program only after the
`production-http-server` soak and first remote CI run are complete, so the host
is not shared.

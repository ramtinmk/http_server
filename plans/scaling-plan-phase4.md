---
plan_id: scaling-phase-4-scale-accept
title: Phase 4 Scale Accept and CPU Work
category: implementation
status: active
owner: agent
created: 2026-09-22
updated: 2026-09-22
related: [scaling-plan]
---

# Phase 4 Scale Accept and CPU Work

- **Phase ID:** `scaling-phase-4-scale-accept`
- **Objective:** Scale accept and CPU work beyond the single event loop only where a profile identifies a limit, and close the Phase 3 saturation/backpressure gap that gates safe accept scaling.
- **Complexity:** 4 — This is a cross-module concurrency change (per-loop ownership, accept distribution, multi-loop shutdown) combined with the absorbed saturation/backpressure work; it stays one bounded phase because multi-loop correctness and saturation behavior must be validated together as an integrated admit/close path. Justification recorded per `plan-spec.md` §7.
- **Risk:** high — Wrong connection ownership or accept distribution can corrupt response ordering, leak connections, or convert overload into a client-visible error storm.

## Purpose

The Phase 3 single event loop already meets the 5,000 req/s acceptance gate.
Phase 4 does not add threads "just because". It first requires a profile that
names the limiter, then scales accept and CPU work only along the direction that
profile supports, and it completes the saturation/backpressure behavior that was
left open at the end of Phase 3. `gzip` is already served from startup-cached
variants and precompression is already implemented, so those two parent-plan
items are recorded as done rather than re-planned.

## Scope

**Work locations:** `src/event_loop.c`, `src/main.c`, `src/http_server.c`, the
event-loop and configuration headers, `src/metrics.c`, `tests/server_test.c`,
and `scripts/http_benchmark.py`. Use the build and test conventions in
`AGENTS.md`; regenerate with `cmake -S . -B .` after adding or removing a C file.

**In:**

- A profiling/decision gate that precedes any concurrency scaling.
- Completion of the parent plan's "Saturation capacity and backpressure
  follow-up" (connection-capacity derivation, connection-table sizing, listener
  backpressure, bounded overload, idle-buffer decoupling, saturation metrics,
  and the below/equal/above-capacity acceptance test).
- Multiple event-loop threads with explicit one-owner-per-connection semantics.
- An explicit accept model, with per-loop `SO_REUSEPORT` listeners as an
  alternative only when measured distribution is poor.
- Keeping cached compression off the event loop and capping any future
  compression concurrency.
- Benchmark/metrics plumbing for the new counters.

**Out:**

- TLS termination, dynamic application execution, and arbitrary-path file
  serving.
- Edge-triggered `epoll`.
- Kernel and deployment tuning; those belong to `scaling-phase-5-os-tuning`.
- Runtime (uncached/dynamic) compression beyond the bounded queue definition.

## Baseline

The accepted Phase 3 evidence is `benchmarks/phase3_acceptance.csv`: a
10-second warmup, 60-second steady-state window, and bounded drain.

| Scenario | Steady responses | Failures | Successful rps | p99 ms |
| --- | ---: | ---: | ---: | ---: |
| `keep-alive-5000` | 299,998 | 0 | 4,999.97 | 1.027 |
| `new-connection-5000` | 300,146 | 0 | 5,002.43 | 10.646 |
| `mixed-paths-5000` | 299,992 | 0 | 4,999.87 | 9.363 |
| `gzip-500` | 30,000 | 0 | 500.00 | 0.722 |
| `error-paths` | 60,000 | 0 | 1,000.00 | 0.811 |
| `slow-clients` | 30,000 | 0 | 500.00 | 0.714 |

The indicative local `wrk -t12 -c400 -d30s` snapshot in `readme.md` reached
131,017 req/s at p50 3.33 ms / p99 5.04 ms. These are the control numbers a
Phase 4 change must not regress.

The parent plan records the unresolved saturation failure: at
`MAX_ACTIVE_CONNECTIONS=512` and `wrk -t12 -c1000`, the server completed only
120.44 req/s with 4,875,861 read errors and 15 timeouts. That is an overload
failure, not a capacity result, and Phase 4 must replace it with a documented
bounded outcome.

The `phase3_acceptance.csv` rows do not contain event-loop wakeup, `EAGAIN`,
partial-write, deadline-close, or output-queue fields, and the benchmark
`CSV_FIELDS` currently export only the blocking-model server counters. Metrics
plumbing is therefore part of the baseline gap this phase closes.

## Measurement contract

For every quantitative check, use the same documented benchmark host, build
flags, resource limits, scenario definitions, and reported fields as
`plans/scaling-plan.md`. Report warmup, steady-state, and drain separately.

The primary acceptance run still offers exactly 5,000 requests/second for 60
seconds and must complete at least 300,000 valid requests with zero transport,
framing, or unexpected-status errors. Scenario p99 limits remain `<20 ms`
keep-alive plain, `<50 ms` new connection, and `<100 ms` gzip. Evidence comes
from the benchmark result CSV and the server metrics snapshot.

In addition, this phase reports, at minimum:

- Per-loop and aggregate wakeups, readable/writable events, `EAGAIN`,
  partial writes, deadline closes, pipeline-full events, and output-drained
  events.
- Effective connection capacity derived at startup, admission rejects by
  reason, overload responses, resets, listener-disabled time, active-connection
  high-water mark, and current/maximum leased buffer bytes.
- `server_cpu_seconds_per_1000_requests` and `hardware_agnostic_rps` for the
  single-loop control and each scaled configuration.

A scaling change is accepted only if it improves a named metric from the
profiling gate without regressing the acceptance criteria; otherwise it is
documented as unjustified and reverted.

## Steps / Work

### A. Profiling decision gate (must precede B–D)

1. [ ] Capture a CPU/system profile of the single event loop under
   `keep-alive-5000`, `new-connection-5000`, and one above-capacity saturation
   run. Attribute time to `accept`, `epoll_wait`, parsing, `send`, and other
   syscalls.
2. [ ] Record the pre-change `el_*` counters, `server_cpu_seconds_per_1000_requests`,
   `hardware_agnostic_rps`, active-connection high-water mark, and CPU
   utilization for the control configuration.
3. [ ] Write a decision record naming the limiter and selecting exactly one
   direction: multi-loop threads, `SO_REUSEPORT`/multi-process, or no scaling
   justified with evidence. Do not proceed to C or E without it.

### B. Close the saturation/backpressure follow-up (absorbed from Phase 3)

1. [ ] Derive the effective connection capacity at startup from the soft
   `RLIMIT_NOFILE`, already-open descriptors, reserved descriptors for the
   listener, epoll, metrics, logs, and shutdown infrastructure, plus an
   operator-configured maximum. Fail clearly or clamp safely; do not rely on a
   warning alone.
2. [ ] Replace the fixed `g_conn_pool[MAX_ACTIVE_CONNECTIONS]` with
   runtime-sized per-loop connection state, or document and enforce the maximum
   supported table size separately from the descriptor limit.
3. [ ] Define listener backpressure: disable listener read interest while the
   connection table is full and re-enable it after a close. Document backlog
   behavior and when a client may still see a connect timeout or refusal.
4. [ ] Provide a bounded overload policy: emit a complete `503 Service
   Unavailable` where safe, otherwise close before accepting request data. Never
   accept and silently reset a request because the configured cap was reached.
5. [ ] Decouple idle keep-alive state from request buffers and response storage;
   release or shrink input/output storage when a connection is idle so memory
   scales with active requests, not table size.
6. [ ] Add saturation metrics: effective capacity, listener-disabled time,
   admission rejects by reason, backlog pressure, overload responses, resets,
   active-connection high-water mark, and current/maximum leased buffer bytes.
7. [ ] Add a fixed-rate saturation acceptance test at `-c` below, equal to, and
   above the configured limit, reporting completed responses,
   connect/read/write/timeout errors, p99 latency, active-connection high-water
   mark, descriptors, RSS, and post-drain values. The above-limit case must have
   a documented bounded outcome rather than a read-error storm.

### C. Multi-threaded event loops (profiling-justified)

1. [ ] Refactor `event_loop_run` into a per-thread `EventLoop` object owning its
   epoll fd, connection table and free list, deadline scan, and shutdown
   `eventfd`. Convert the existing file-scope `g_conn_pool`, `g_free_list`, and
   `g_listen_sentinel` into per-loop state.
2. [ ] Enforce one owner per connection: the owning loop alone touches the
   socket, parser, buffers, output queue, and timer. Define the handoff protocol
   if any cross-loop transfer is introduced; otherwise forbid it.
3. [ ] Select an accept model and record why:
   - Default: a dedicated acceptor distributes accepted fds to per-loop epolls
     (for example a dispatch ring or per-loop `eventfd`), which avoids the
     accept thundering herd and keeps capacity accounting global.
   - Alternative: per-loop listening sockets with `SO_REUSEPORT`, used only if
     measured kernel hashing distributes load acceptably across loops.
4. [ ] Add `EL_THREAD_COUNT` in `server_config.h`, default `1` so the control
   path is preserved, and print it in startup output. Choose the production
   value by measurement around the available CPU count, not by assumption.
5. [ ] Make shutdown multi-loop safe: wake every loop, stop accepting, drain or
   close connections exactly once, and join cleanly. Do not leave two owners for
   one socket.
6. [ ] Ensure metrics aggregation across loops is lock-free or otherwise
   off the hot path, and that admission/connection accounting is global rather
   than per-loop when the configured maximum is process-wide.

### D. CPU work and zero-copy

1. [ ] Confirm precompression and cached-variant selection still happen only at
   startup and that startup fails fast when an asset cannot be loaded (already
   implemented; verify and cover with a test).
2. [ ] Keep gzip off the event loop. If dynamic compression is ever added, define
   a bounded compression queue, cap concurrent compressors, apply backpressure,
   define queue-full behavior, and never let a slow client retain compression
   buffers indefinitely.
3. [ ] Evaluate `sendfile` only after measuring in-memory body-copy cost. For
   cached response bodies, `send()`/`writev()` is expected to remain adequate;
   if `sendfile` is added, retain per-connection file offset and nonblocking
   `EAGAIN`/partial-progress handling behind a configuration flag.

### E. Metrics and benchmark plumbing

1. [ ] Export the existing `el_*` counters plus the new saturation counters into
   `scripts/http_benchmark.py` `CSV_FIELDS` as trailing additions, aggregated
   across loops, preserving CSV backward compatibility.
2. [ ] Include the same fields in the `HTTP_SERVER_METRICS_FILE` snapshot.
3. [ ] Add the above-limit saturation run to the benchmark coverage and record
   its results.

### F. Tests

1. [ ] Multi-loop correctness: concurrent connections are accepted, served, and
   closed without cross-loop interference; keep-alive and pipelining preserve
   response order within a connection.
2. [ ] Saturation at `-c` below, equal to, and above capacity produces the
   documented bounded outcome and returns active connections, buffers, tasks,
   descriptors, and RSS to baseline after drain.
3. [ ] Listener interest is disabled while the table is full and re-enabled after
   a close.
4. [ ] Existing ring, thread-pool, and server suites remain green.

## Validation

- [ ] Run `make` and confirm `bin/http_server` and `bin/run_tests` rebuild;
  regenerate with `cmake -S . -B .` if C files were added.
- [ ] Run `./bin/run_tests ring` and `./bin/run_tests thread_pool`.
- [ ] Start `./bin/http_server` from the repository root and run
  `./bin/run_tests server`; include multi-loop cases with `EL_THREAD_COUNT > 1`.
- [ ] Run the fixed-rate acceptance scenarios (`new-connection-5000`,
  `keep-alive-5000`, `mixed-paths-5000`, `gzip-500`) with the Phase 4 contract:
  bounded warmup, exactly 60 seconds at the offered rate, bounded drain, and
  separate phase reporting.
- [ ] Run the saturation acceptance at `-c` below, equal to, and above the
  configured limit; evidence in the benchmark CSV and metrics snapshot.
- [ ] Run repeated connect/disconnect and saturation campaigns long enough to
  show active connections, buffers, output queues, descriptors, tasks, and RSS
  return to baseline after drain.
- [ ] Compare the control (`EL_THREAD_COUNT=1`) and each scaled configuration
  using the profiling gate metrics and the acceptance criteria.

## Exit criteria

- [ ] A recorded profile identifies the single-loop limiter, and every scaling
  change cites that evidence.
- [ ] The saturation/backpressure follow-up is complete: effective capacity is
  derived and enforced, listener backpressure is implemented, overload responses
  are bounded, idle connections release request/response storage, and saturation
  metrics are present.
- [ ] The above-limit saturation run has a documented bounded outcome with no
  unexplained read-error storm.
- [ ] Multiple event-loop threads (when enabled) preserve strict response
  ordering per connection and leak no connection, buffer, or descriptor.
- [ ] `new-connection-5000`, `keep-alive-5000`, `mixed-paths-5000`, and
  `gzip-500` still meet the 5,000 req/s, zero-unexpected-error, and
  scenario-specific p99 criteria on the benchmark host.
- [ ] No unbounded growth in active connections, memory, file descriptors, or
  queued work during warmup, steady state, or drain.
- [ ] `SO_REUSEPORT`/multi-process is either implemented behind a flag with
  measured benefit or explicitly deferred with the profiling evidence recorded.
- [ ] All existing HTTP correctness tests remain green.

## Risks and rollback

- **Accept thundering herd across loops** → Use a dedicated acceptor or
  `EPOLLEXCLUSIVE`/`SO_REUSEPORT` with measured distribution; record repeated
  wakeups and revert the accept model if CPU rises without added throughput.
- **Cross-loop connection ownership bugs** → Keep per-loop state, make close
  idempotent, and forbid direct cross-loop field access; add a stress test that
  runs `EL_THREAD_COUNT > 1` under keep-alive and pipelining.
- **Unbounded memory or descriptor growth** → Enforce effective capacity and
  buffer limits before allocation; rerun the leak campaign after every change.
- **Overload regressions** → Preserve the close-on-admission-failure control
  behavior, then add backpressure and bounded `503` incrementally, with the
  above-limit acceptance test as the gate.
- **Performance regression despite correctness** → Keep the Phase 3
  `EL_THREAD_COUNT=1` configuration as the control and benchmark output;
  revert the scaling change while retaining tests and metrics, then use the
  profile to retry.

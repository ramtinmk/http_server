---
plan_id: scaling-phase-4-profiling
title: Phase 4 Profiling Decision Gate
category: spike
status: done
owner: agent
created: 2026-09-22
updated: 2026-09-24
related: [scaling-phase-4-scale-accept, scaling-phase-4-core-autoscale]
---

# Phase 4 Profiling Decision Gate

- **Question:** Does the Phase 3 single event loop limit throughput at the
  acceptance rate on this host, and which scaling direction (if any) does the
  profile justify before any multi-loop work is enabled by default?
- **Timebox:** One session. Profile the single-loop control under keep-alive and
  new-connection load plus a saturation run, then decide.
- **Method:** Measure the `EL_THREAD_COUNT=1` control with the existing
  dependency-free harness (`scripts/http_benchmark.py`), which samples server CPU
  from `/proc/<pid>/stat` and reads the `HTTP_SERVER_METRICS_FILE` snapshot.
  Attribute syscall time with `strace -c -f -p <pid>`. Then measure
  `EL_THREAD_COUNT=4` accept distribution from the per-loop diagnostics added to
  `src/event_loop.c`.

## Environment

- Host: 8 vCPU, WSL2, Linux; `ulimit -n` soft/hard = 1048576.
- Build: default `make` (C11/gnu11, `USE_EVENT_LOOP=1`).
- Control: `EL_THREAD_COUNT=1`, effective capacity 1024
  (`operator_max=1024`, descriptor cap 1048512).
- Load generator: `scripts/http_benchmark.py` (single client process; not
  `wrk`, which is not installed on this host).

## Findings

### Single-loop control throughput and CPU

| Run | offered rps | successful rps | server CPU | client CPU | p99 ms | `limited_by` |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| keep-alive, 5 s | 3000 | 3000.2 | 23.06% | 50.83% | 2.80 | neither/unknown |
| new-connection, 5 s | 3000 | 1432.0 | 16.44% | 143.84% | 24.49 | client |
| keep-alive, 5 s | 15000 | 3647.4 | 24.96% | 142.33% | 17.32 | client |

The single loop never exceeds roughly a quarter of one CPU on this host, and the
achieved rate tracks client CPU, not server CPU. The Python generator saturates
one or more client cores (`client_cpu` 143%) well before the server loop does, so
the observed limiter for new-connection and high-rate keep-alive runs is the
**load generator**, not `epoll_wait`/accept/parsing.

### Control counters (`HTTP_SERVER_METRICS_FILE`, keep-alive run)

| Field | Value |
| --- | ---: |
| `connection_capacity` | 1024 |
| `active_connections` / `active_connections_max` | 16 / 16 |
| `el_wakeups` | 35321 |
| `el_readable_events` | 17838 |
| `el_writable_events` | 17837 |
| `el_eagain` | 17837 |
| `el_partial_writes` | 0 |
| `el_output_drained` | 17661 |
| `el_connections_opened` / `closed` | 193 / 177 |
| `buffer_bytes_current` / `max` | 0 / 72828 |
| `listener_disabled_count` / `_ms` | 0 / 0 |
| `admission_rejected` / `overload_responses` / `connection_resets` | 0 / 0 / 0 |

Idle keep-alive connections release their input buffers (`buffer_bytes_current`
returns to 0), so memory scales with active requests rather than table size.

### Syscall attribution (`strace -c -f`, keep-alive burst)

| syscall | % time | calls | errors |
| --- | ---: | ---: | ---: |
| `clock_nanosleep` | 58.69 | 14 | – |
| `sendto` | 18.91 | 9332 | – |
| `recvfrom` | 8.99 | 9408 | 4680 |
| `epoll_ctl` | 8.78 | 9393 | – |
| `epoll_wait` | 0.75 | 588 | – |
| `accept4` | 0.12 | 90 | 42 |

`clock_nanosleep` is the metrics-reporter thread and is an artifact of tracing a
mostly idle process; excluding it, the profile is dominated by data-path syscalls
(`send`/`recv`) and per-request `EPOLL_CTL_MOD` interest updates. `accept` is
negligible. `strace` inflates absolute syscall cost, so this is a relative
attribution only.

### Multi-loop accept distribution (`EL_THREAD_COUNT=4`)

400 new connections: loop 0 = 121, loop 1 = 94, loop 2 = 84, loop 3 = 101
(300–400 wakeups each). Kernel `SO_REUSEPORT` hashing distributes acceptably; no
dedicated userspace acceptor is needed.

## Decision

**Direction: multi-loop event loops, provided as an opt-in behind
`EL_THREAD_COUNT`, with per-loop `SO_REUSEPORT` listeners. The default remains
`EL_THREAD_COUNT=1`.**

Rationale:

- The profile does **not** show the single loop as the limiter at any rate this
  host can generate; server CPU stays at or below ~25% and `accept` is ~0.12% of
  syscall time. Scaling the default would not improve the acceptance metrics and
  would add concurrency risk for no measured gain.
- Multi-loop is still implemented and validated (`EL_THREAD_COUNT>1`, 24/24
  server tests) because the parent plan calls for the capability and a CPU-bound
  deployment can enable it by build flag. Per-loop `SO_REUSEPORT` was selected
  over a dedicated acceptor because it avoids fd handoff and measured balanced.
- The multi-process / single-listener `SO_REUSEPORT` alternative is deferred:
  it would need cross-process shared admission accounting and metrics.

Follow-up (not a scaling change): `EPOLL_CTL_MOD` is issued on every
read↔write transition (~9% of traced syscall time). Skipping the `epoll_ctl`
when the interest set is unchanged is a bounded micro-optimization to evaluate
separately; it does not justify scaling.

## Consequences

- `EL_THREAD_COUNT` defaults to 1; production value is chosen by measurement on
  a host whose client can actually saturate the loop.
- The saturation/backpressure follow-up is the phase's real acceptance gate; the
  above-capacity bounded outcome is verified by `scripts/saturation_test.py`.
- `sendfile` remains deferred: bodies are served from startup-cached memory and
  the profile shows `send`/`recv` as ordinary data-path cost, not a copy hotspot.

## Addendum (2026-09-24)

The decision above chose an opt-in default (`EL_THREAD_COUNT=1`) because the
single loop was not the limiter on this host. The default was subsequently
changed to host-sized auto-detection (`EL_THREAD_COUNT=0` → one loop per online
CPU core) in `scaling-phase-4-core-autoscale`, matching how production
event-driven servers size their worker/loop count. This addendum does not
invalidate the profiling result: the control for benchmark comparisons remains
`EL_THREAD_COUNT=1`, and the override still allows reproducing the profiled
configuration exactly. The multi-process `SO_REUSEPORT` alternative remains
deferred as recorded above.

---
plan_id: production-http-server-phase0-ops
title: Phase 0 operational safety (config, runtime tuning, lifecycle, logging)
category: implementation
status: done
owner: agent
created: 2026-09-29
updated: 2026-09-29
related: [production-http-server, production-http-server-phase0-overload]
---

# Phase 0 operational safety (config, runtime tuning, lifecycle, logging)

## Purpose

Finish the remaining Phase 0 work items of `production-http-server` so the
server can run unattended: one validated configuration source (file + env +
CLI), runtime-tunable keep-alive/timeout/capacity limits, a graceful
`SIGTERM`/`SIGINT` drain with `sd_notify`, and leveled JSON access/error
logging whose hot path cannot block an event loop. The overload policy itself
was completed by `production-http-server-phase0-overload`.

## Scope

In: `src/main.c`, `src/event_loop.c`, new `src/config.c`, `src/log.c`,
`src/sd_notify.c`, their headers, `include/server_config.h`, the E2E harnesses
under `scripts/`, and the architecture/environment/gotcha/readme docs.

Out: TLS, document-root serving, HTTP parser correctness, Prometheus metrics,
systemd unit packaging, and the compile-time `EL_THREAD_COUNT` (stays a
compile-time override by project convention).

## Steps

1. [ ] Add `config.{c,h}`: defaults from `server_config.h`, then a key=value
   file (`HTTP_SERVER_CONFIG`/`--config`), then environment, then CLI; validate
   ranges and reject unknown/contradictory values naming the key.
2. [ ] Thread the resolved `ServerConfig` through `main` and the event loop so
   `max_connections`, `max_keepalive_requests`, input cap, and the three
   timeouts are runtime-tunable; keep `EL_THREAD_COUNT` compile-time.
3. [ ] Add `log.{c,h}`: leveled JSON lines to a file or stderr, a nonblocking
   writer thread fed by an `O_NONBLOCK` pipe (drop-on-full), access logs that
   honor `HTTP_SERVER_ACCESS_LOG`, and `SIGHUP` file reopen.
4. [ ] Add `sd_notify.{c,h}` (dependency-free `NOTIFY_SOCKET` datagram) and a
   graceful drain: stop accepting, finish in-flight responses within
   `shutdown_drain_timeout`, then exit cleanly.
5. [ ] Extend the E2E tooling with an invalid-config check, a `SIGTERM`
   drain-under-load check, an access-log overhead comparison, and a 2 x
   capacity `wrk` run; commit the JSON/CSV artifacts.
6. [ ] Update `docs/architecture.md`, `docs/env-vars.md`, `docs/gotchas.md`,
   `readme.md`, the `AGENTS.md` files, and the `production-http-server` Phase 0
   checklist.

## Validation

- [x] `cmake -S . -B . && make` (new `src/*.c` files require the reconfigure).
- [x] `./bin/run_tests ring` and the `server` suite per `AGENTS.md`.
- [x] `python3 scripts/phase0_lifecycle_test.py` (invalid config + drain).
- [x] `python3 scripts/saturation_test.py --capacity 16` still passes.
- [x] `python3 scripts/phase0_capacity_2x.py` writes the above-capacity
   artifact with zero client timeouts and bounded 503s.

## Exit criteria

- [x] An invalid key exits non-zero naming the exact key, and the effective
  configuration is recorded in the startup log; evidence:
  `benchmarks/production_phase0_lifecycle.json`.
- [x] `SIGTERM` under full keep-alive load drains within the deadline with zero
  truncated responses and a zero exit status; evidence:
  `benchmarks/production_phase0_lifecycle.json`.
- [x] At `2 x` capacity: zero client timeouts, bounded `503`s, bounded
  `listener_disabled_count`; evidence:
  `benchmarks/production_phase0_2x.json`.
- [x] Access logging adds < 5% latency overhead on the keep-alive scenario and
  never blocks an event loop (nonblocking pipe, drop counter); evidence:
  `benchmarks/production_phase0_accesslog.json`.

## Risks and rollback

- Config parsing regressions block startup -> config defaults reproduce the
  previous compile-time behavior exactly; a missing file is not an error.
- Runtime limits widen the hot path -> resolve once into the loop and snapshot
  the effective values in metrics/startup output.
- Drain deadlocks or truncates -> bound the drain by
  `shutdown_drain_timeout` and force-close remaining connections after it.
- Logging stalls a loop -> `O_NONBLOCK` pipe with atomic `PIPE_BUF` writes and
  a visible drop counter; a full pipe degrades to dropped access lines.

---
plan_id: production-http-server-phase5
title: Phase 5 observability and operations
category: implementation
status: active
owner: agent
created: 2026-10-02
updated: 2026-10-02
related: [production-http-server, production-http-server-phase4]
---

# Phase 5 observability and operations

## Purpose

Give operators a view of health, latency, and errors, and a way to reload safe
settings without downtime. This is the `production-http-server` phase 5 phase.
It is additive: the internal JSON metrics snapshot used by the benchmark
harness stays unchanged; the new surface is a Prometheus endpoint, structured
logs with request IDs, health/readiness probes, and a widened `SIGHUP` reload.

## Scope

In: a Prometheus text endpoint and latency histogram in `src/metrics.c`; health
and readiness endpoints and request IDs in `src/http_server.c` /
`src/event_loop.c`; optional syslog and log-level reload in `src/log.c`; new
runtime keys in `include/config.h`, `include/server_config.h`, and `src/config.c`;
readiness flipping during shutdown; an E2E harness
`scripts/phase5_observability_test.py` and a `phase5-observability` target; and
docs.

Out: a separate admin listener/port (endpoints share the data listeners and are
gated by one switch; a dedicated admin port is a Phase 6 deployment item),
distributed tracing, OpenTelemetry, runtime reload of per-connection timeouts
(they stay restart-only and are documented as such), and any change to the
internal snapshot format or benchmark harness.

## Baseline

- `src/metrics.c` renders a flat one-line JSON snapshot to
  `HTTP_SERVER_METRICS_FILE`; there is no HTTP endpoint and no latency
  histogram.
- `src/log.c` emits leveled server lines and access lines with no request ID and
  no syslog target; `SIGHUP` only reopens the log file.
- `src/http_server.c` routes fixed aliases and the document root only; there is
  no health/readiness path.
- Evidence: `src/metrics.c`, `src/log.c`, `src/http_server.c`,
  `docs/architecture.md`.

## Workstreams

### 5a Prometheus metrics endpoint

**Work**

- [x] Latency histogram with fixed buckets (`metrics_observe_request_latency`)
  plus a start-time/uptime clock, rendered lock-free.
- [x] `metrics_prometheus()` producing the text exposition format with
  `# HELP`/`# TYPE` for requests, responses by status class, the request
  duration histogram, connections, timeouts/admission, TLS, cache, process RSS,
  and readiness.
- [x] Serve `/metrics` (configurable path) as `text/plain; version=0.0.4`,
  heap-owned body, bounded output, `Cache-Control: no-store`.
- [x] Add `observability`, `metrics_path`, `health_path`, `readiness_path`,
  `syslog` runtime keys (defaults/flags/env/validation).

### 5b Structured logs and request IDs

**Work**

- [x] Generate a per-request ID at parse time and carry it on
  `PendingResponse`; emit it in the access log and as an `X-Request-Id`
  response header.
- [x] Optional syslog target: when `syslog` is enabled, the writer thread also
  forwards each complete record to syslog(3); log reopen is unchanged.
- [x] `SIGHUP` re-reads the runtime log level (safe tunable) in addition to
  reopening the file; document the reloadable set.

### 5c Health and readiness

**Work**

- [x] `/healthz` (liveness) always `200` while the process serves; `/readyz`
  (readiness) `200` when accepting and `503` once shutdown begins.
- [x] Readiness state is an atomic set at startup and cleared from the shutdown
  signal handler before the drain, so probes fail before connections stop.

### 5d Lifecycle logging

**Work**

- [x] Log a startup `ready` record alongside `sd_notify(READY)` and a shutdown
  record alongside `sd_notify(STOPPING)` so logs and systemd notify correlate.

### 5e Tests and docs

**Work**

- [x] `scripts/phase5_observability_test.py` E2E: starts the server with
  observability on, asserts `/healthz` 200, `/readyz` 200, `/metrics` parses and
  exposes the documented names, `X-Request-Id` is present and appears in the
  access log, and a `SIGHUP` on an open keep-alive connection drops zero
  connections. Writes `benchmarks/production_phase5_observability.json`.
- [x] Update `docs/architecture.md`, `docs/env-vars.md`, `docs/gotchas.md`,
  `readme.md`, and the program progress snapshot; document alert thresholds and
  a minimal dashboard query set
  (`docs/runbooks/observability-and-reload.md`).

## Steps (this iteration)

1. [x] Add the latency histogram, start time, and `metrics_prometheus()` to
   `src/metrics.c` + `include/metrics.h`.
2. [x] Add new runtime keys to `include/server_config.h`, `include/config.h`,
   `src/config.c`.
3. [x] Add `request_id` to `PendingResponse`; generate and propagate it; add the
   `X-Request-Id` header and access-log field.
4. [x] Add observability endpoint routing + readiness to `src/http_server.c` /
   `include/http_server.h`.
5. [x] Add syslog + runtime log level to `src/log.c` / `include/log.h`; wire the
   `SIGHUP` log-level reload (via `config_reload_if_requested`).
6. [x] Set/clear readiness in `src/main.c` and `src/event_loop.c`; add lifecycle
   log lines.
7. [x] Add `scripts/phase5_observability_test.py`, the `phase5-observability`
   target, and docs.

## Validation

- [x] `cmake -S . -B . && make` with no new warnings (the only warnings are the
  pre-existing `tls_test.c` `snprintf` truncation notices on `main`).
- [x] `./bin/run_tests ring`, `./bin/run_tests server` (doc root `.`) — 30/30,
  `./bin/run_tests tls` — 6/6.
- [x] `python3 scripts/phase5_observability_test.py` exits 0 and writes
  `benchmarks/production_phase5_observability.json`.

## Exit criteria

- [x] A scrape of `/metrics` returns the documented names with valid Prometheus
  text; evidence: `benchmarks/production_phase5_observability.json` `metrics`.
- [x] Every access-log line carries the same `request_id` returned in the
  response `X-Request-Id` header; evidence: `request_id` block of the artifact.
- [x] `SIGHUP` on an open keep-alive connection applies the new log level and
  leaves the connection usable with zero dropped connections; evidence:
  `reload` block of the artifact.
- [~] `/healthz` serves `200` while accepting; `/readyz` serves `200` while
  accepting and the probe fails once shutdown removes the listener (observed as
  connection-refused). A drain-time `503` is not observable because accepting
  stops at the same instant. Evidence: `health`/`lifecycle` blocks.
- [x] No regression in the ring/server/TLS suites.

## Risks and rollback

- **Metrics endpoint leaks sensitive figures or is scraped publicly** → the
  whole surface is off unless `observability_enabled` is set, paths are
  configurable, and docs tell operators to firewall or bind it to an admin
  network; rollback is disabling the key.
- **Handler output is unbounded** → rendering is capped at a fixed buffer size
  and truncates rather than allocating per scrape.
- **Request ID work perturbs the hot path** → one relaxed atomic increment and a
  small `snprintf` at parse time; no locks, no I/O.
- **Log level reload races a producer** → the level is a plain atomic read on
  the hot path and a single atomic store on reload.
- **Readiness flip races the drain** → the flag is set from the
  async-signal-safe handler with an atomic store and is only consulted on the
  next request.

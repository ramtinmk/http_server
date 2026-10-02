# Runbook: observability and reload

How to expose health, readiness, and Prometheus metrics, and how to reload safe
settings without downtime. Configuration reference: `docs/env-vars.md`.

## 1. Enable the endpoints

The endpoints are off by default and share the plaintext/TLS data listeners.
Turn them on with one key:

```ini
observability = 1
metrics_path = /metrics      # default
health_path = /healthz       # default
readiness_path = /readyz     # default
```

There is no authentication and no separate admin port in this phase, so restrict
access at the network layer (firewall, security group, or a loopback-only
reverse proxy). The configured paths are matched **before** document-root
resolution, so keep them clear of real files.

## 2. What each endpoint means

- `GET /healthz` — liveness. Always `200` with body `ok` while the process is
  running. A failure here means the process is gone.
- `GET /readyz` — readiness. `200` with body `ready` while accepting; `503` with
  body `draining` once shutdown has begun. Because the shutdown path removes the
  listeners almost immediately, a probe may instead observe connection-refused
  during the drain; both are "not ready".
- `GET /metrics` — Prometheus text (version 0.0.4). `HEAD` returns the headers
  with no body; other methods get `405`.

Only `GET`, `HEAD`, and `OPTIONS` are served on these paths, like every other
resource.

## 3. Scrape and verify

```bash
curl -s http://127.0.0.1:8081/healthz        # ok
curl -s http://127.0.0.1:8081/readyz         # ready
curl -s http://127.0.0.1:8081/metrics | head # HELP/TYPE + samples
```

Prometheus scrape config:

```yaml
scrape_configs:
  - job_name: http_server
    static_configs:
      - targets: ["127.0.0.1:8081"]
```

Key series: `simplehttp_requests_total`, `simplehttp_responses_total{status}`,
`simplehttp_request_duration_seconds_{bucket,sum,count}`,
`simplehttp_active_connections`, `simplehttp_connection_capacity`,
`simplehttp_admission_rejected_total`, `simplehttp_overload_responses_total`,
`simplehttp_header_timeouts_total`, `simplehttp_tls_handshakes_total`,
`simplehttp_tls_handshake_failures_total`, `simplehttp_cache_bytes`,
`simplehttp_ready`, and `simplehttp_uptime_seconds`. The memory gauges are `0`
unless `HTTP_SERVER_METRICS_FILE` started the reporter thread, which is the only
memory sampler.

Run the end-to-end gate:

```bash
make phase5-observability
# artifact: benchmarks/production_phase5_observability.json
```

## 4. Correlate a request

Every response carries `X-Request-Id`, and the access record carries the same
value in `request_id`:

```bash
curl -sI http://127.0.0.1:8081/hello | grep -i x-request-id
# X-Request-Id: 0006abfca8901ccf
grep 0006abfca8901ccf /var/log/http_server.jsonl
```

The access record also has `latency_us`, `status`, and `bytes`, so a slow or
failed request can be traced from the client through the log.

## 5. Reload without downtime

`kill -HUP <pid>` (or `systemctl reload`) does all of the following atomically
from the running server's point of view, dropping zero connections:

- reopens `log_file` (logrotate);
- reloads `tls_cert_file`/`tls_key_file` when TLS is enabled;
- re-reads the configuration surface and applies a new `log_level`.

Everything else in the configuration is **not** reloadable (ports, capacity,
timeouts, keep-alive limit, document root, TLS toggle, observability paths).
Change those and restart. A malformed configuration makes the reload fail and
keeps the previous settings; the failure is logged as
`config reload failed; keeping previous`.

## 6. Alert thresholds (starting points)

Tune to your traffic; these are sane defaults, not guarantees.

| Alert | Condition | Why |
|-------|-----------|-----|
| Not ready | `simplehttp_ready == 0` for 30 s | process shutting down or stuck. |
| Elevated 5xx | `rate(simplehttp_responses_total{status="500"}[5m]) > 0` | server errors. |
| Overload | `rate(simplehttp_overload_responses_total[5m]) > 0` | capacity reached; `503`s are being served. |
| Admission rejections | `rate(simplehttp_admission_rejected_total[5m]) > 0` | connections refused at capacity. |
| Slow requests | `histogram_quantile(0.99, rate(simplehttp_request_duration_seconds_bucket[5m])) > 1` | p99 latency above 1 s. |
| TLS failures | `rate(simplehttp_tls_handshake_failures_total[5m]) > 1` | misconfiguration or hostile handshakes. |
| Header timeouts | `rate(simplehttp_header_timeouts_total[5m]) > 1` | slowloris or unhealthy clients. |
| Log drops | `dropped_logs` in the shutdown record, or `syslog`/log gaps | the logging pipe overflowed. |

Minimal dashboard panels: request rate, p50/p99 from the histogram, response
classes (2xx/4xx/5xx), active vs capacity connections, overload/admission rate,
TLS handshake rate and failures, cache bytes, and `simplehttp_ready`.

## 7. Structured logs and syslog

Records are JSON, one object per line, and include `ts`, `level`, `kind`, and a
`msg` (`kind: access` adds `request_id`, `client`, `method`, `path`, `status`,
`bytes`, `latency_us`). Set `syslog = 1` to mirror the same records to
`syslog(3)` (ident `http_server`, facility `daemon`) in addition to the file.

Logging never blocks an event loop: producers write to a nonblocking pipe and a
full pipe drops records, counted in `dropped_logs`. If records are missing,
check for that counter before assuming the request never arrived.

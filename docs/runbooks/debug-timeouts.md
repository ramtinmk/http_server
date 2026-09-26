# Runbook: debug a connection timeout or close

Timeouts are enforced by a per-connection monotonic deadline scanned every
`EL_DEADLINE_SCAN_MS` (100 ms). The reason a connection closed is recorded in
`ELCloseReason` (`include/event_loop.h:21`) and surfaced as metrics counters.

## 1. Reproduce with metrics on

```bash
HTTP_SERVER_METRICS_FILE=/tmp/metrics.json ./bin/http_server
```

The reporter rewrites `/tmp/metrics.json` every 250 ms. Watch the relevant
counter:

| Symptom | Counter | Config |
|---------|---------|--------|
| Client connected but never sent headers | `header_timeout` | `HEADER_READ_TIMEOUT_SEC` (5 s) |
| Keep-alive connection went quiet | `idle_timeout` | `IDLE_TIMEOUT_SEC` (30 s) |
| Client stopped reading mid-response | `write_timeout` | `WRITE_TIMEOUT_SEC` (10 s) |
| Client sent more than the buffer cap | `input_buffer_limit` | `MAX_INPUT_BUFFER_BYTES` (64 KB) |
| Connection closed by the scanner | `el_deadline_close` | aggregate |
| Capacity/backpressure, not a timeout | `rejected_connections`, `listener_disabled`, `backlog_depth_max` | `HTTP_SERVER_MAX_CONNECTIONS` |

## 2. Reproduce deterministically

The E2E suite already encodes each case in `tests/server_test.c`:

- `test_slow_client_header_timeout`
- `test_keepalive_request_limit`
- `test_input_buffer_limit`
- `test_long_header_line_rejected` / `test_long_request_line_rejected`

Run one by name via the suite filter (`./bin/run_tests server` with the server
running; `tests/server_test.h` documents the filter argument), or mirror the
existing test's raw-socket script.

## 3. Distinguish timeout from overload

- A timeout means the deadline expired; the close reason is `CLOSE_DEADLINE`
  and the matching counter increments.
- Overload closes/resets are `CLOSE_BUFFER_FULL`/`CLOSE_SHUTDOWN` paths and bump
  `metrics_listen_drops`/accept-error counters or `metrics_overload_response`.
  Check `rejected_connections` and `connection_capacity` in the snapshot before
  blaming a timeout.
- Header-size rejections (`431`/`414`) come from parser limits
  (`MAX_HEADERS`, `MAX_HEADER_LEN` in `src/http_server.c`), not timeouts.

## 4. If the deadline never fires

The scanner runs inside the per-loop `event_loop_main()` iteration
(`src/event_loop.c:804`). If deadlines stop closing connections while requests
still work, check that the loop is not blocked in `epoll_wait` without the
timeout argument, and that `deadline_set()` is called on every state transition
(accept → header read; request served → idle; response queued → write).

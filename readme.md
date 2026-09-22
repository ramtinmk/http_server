# C HTTP Server

A small HTTP/1.1 static-file server written in C11, using POSIX sockets,
`epoll`, POSIX threads, and `zlib`. The project doubles as a study of
concurrency models — it grew from a blocking `fork()` server, to a thread pool,
to the nonblocking event loop that is the default today — and as a reproducible
benchmark target.

## Architecture

- **Default dispatch:** nonblocking `epoll` event loop (`USE_EVENT_LOOP=1` in
  `include/server_config.h`). `EL_THREAD_COUNT` loops each own a `SO_REUSEPORT`
  listener so the kernel hashes new connections across them (default: 4).
- **Legacy dispatch:** build with `USE_EVENT_LOOP=0` to run the Phase 2 blocking
  thread pool instead.
- **HTTP:** HTTP/1.1 keep-alive with Content-Length framing, request pipelining
  (up to `MAX_PIPELINE_DEPTH`, default 16), and error responses for
  `400`, `404`, `413`, and `501`.
- **Static assets:** `home.html` and `hello.html` are read once at startup and
  cached in memory, both plain and gzip-precompressed. Gzip is served when the
  client sends `Accept-Encoding: gzip`.
- **Resource guards:** bounded active connections, per-connection input cap,
  keep-alive request cap, and read/idle/write timeouts (see
  `include/server_config.h` for every value and its default).
- **Metrics:** set `HTTP_SERVER_METRICS_FILE=<path>` to emit periodic runtime
  snapshots (connections, CPU/RSS, queue depth, rejected tasks, retransmits).

## Build

```
make
```

CMake writes executables to `bin/`:

```
./bin/http_server     # server
./bin/run_tests       # C test runner
```

## Run

The server must be launched from the repository root because it opens
`home.html` and `hello.html` via relative paths. It listens on port `8081`.

```
./bin/http_server
```

| Path     | Response                     |
|----------|------------------------------|
| `/`      | `home.html` (200)            |
| `/home`  | `home.html` (200)            |
| `/hello` | `hello.html` (200)           |
| other    | 404                          |

## Tests

Unit suites do not need a running server:

```
./bin/run_tests ring
./bin/run_tests thread_pool
ctest --output-on-failure
```

The server suite requires `./bin/http_server` to be running from the project
root:

```
./bin/http_server &
SERVER_PID=$!
sleep 0.5
./bin/run_tests server
kill "$SERVER_PID" && wait "$SERVER_PID" 2>/dev/null || true
```

## Benchmarking

Two complementary harnesses are available: a raw `wrk` snapshot for peak
throughput/latency, and the in-repo Python harness that enforces the
hardware-agnostic measurement contract in `plans/scaling-plan.md`.

### `wrk` results

All numbers below were collected locally against a running Phase 4 epoll server
(default `EL_THREAD_COUNT=4`, connection capacity 1024), serving the tiny
cached `home.html`/`hello.html` assets over loopback. Each configuration was
run twice and the higher requests/sec run is reported.

Host under test:

| Item      | Value                                                        |
|-----------|--------------------------------------------------------------|
| CPU       | 13th Gen Intel Core i9-13900K (32 logical CPUs)              |
| Memory    | 31 GiB                                                       |
| OS        | Ubuntu 26.04 LTS (WSL2, kernel 6.18)                         |
| `wrk`     | `debian/4.1.0-4build3 [epoll]`                               |
| Server    | `bin/http_server` @ commit `3fd4155`, `EL_THREAD_COUNT=4`    |

#### Thread count sweep

`wrk -t<N> -c100 -d10s --latency http://127.0.0.1:8081/home`

| Threads | Requests/sec | p50    | p99    | Max     | Timeouts |
|--------:|-------------:|-------:|-------:|--------:|---------:|
| 1       | 137,507      | 408 µs | 1.27 ms | 6.31 ms | 53       |
| 2       | 242,522      | 211 µs | 0.83 ms | 6.55 ms | 0        |
| 4       | 572,677      | 93 µs  | 1.35 ms | 10.12 ms | 0       |
| 8       | 751,057      | 108 µs | 1.45 ms | 10.01 ms | 0       |
| 16      | 658,519      | 128 µs | 1.82 ms | 11.50 ms | 96      |

Throughput peaks around 8 `wrk` threads; beyond that the client and loopback
overhead dominate and the numbers regress.

#### Connection count sweep

`wrk -t4 -c<N> -d10s --latency http://127.0.0.1:8081/home`

| Connections | Threads | Requests/sec | p50    | p99     |
|------------:|--------:|-------------:|-------:|--------:|
| 1           | 1       | 34,980       | 26 µs  | 365 µs  |
| 10          | 4       | 259,487      | 25 µs  | 0.99 ms |
| 50          | 4       | 521,712      | 51 µs  | 0.96 ms |
| 100         | 4       | 531,444      | 96 µs  | 1.16 ms |
| 400         | 4       | 566,405      | 370 µs | 1.52 ms |
| 800         | 4       | 575,358      | 736 µs | 2.45 ms |

`-c1` requires `-t1` in `wrk` and reflects single-connection round-trip cost.
With keep-alive, throughput saturates by ~50 connections while latency grows
with queueing.

#### Keep-alive, path, and compression

`wrk -t4 -c100 -d10s --latency <headers> http://127.0.0.1:8081<path>`

| Setting               | Path     | Request header(s)                        | Requests/sec | p50    | p99     |
|-----------------------|----------|------------------------------------------|-------------:|-------:|--------:|
| keep-alive            | `/home`  | —                                        | 531,444      | 96 µs  | 1.16 ms |
| keep-alive            | `/hello` | —                                        | 538,955      | 95 µs  | 1.23 ms |
| keep-alive + gzip     | `/hello` | `Accept-Encoding: gzip`                  | 545,803      | 94 µs  | 1.26 ms |
| new conn/request      | `/home`  | `Connection: close`                      | 138,885      | 160 µs | 2.21 ms |
| new conn/request      | `/home`  | `Connection: close` (`-c400`)            | 143,802      | 500 µs | 3.06 ms |
| new conn/request+gzip | `/hello` | `Connection: close`, `Accept-Encoding: gzip` | 142,221  | 160 µs | 2.04 ms |

Reusing connections is roughly **3.8×** faster than a fresh TCP connection per
request. On these sub-300-byte assets, gzip (`249` → `179` bytes) is
latency-neutral.

#### HTTP pipelining

`wrk -t4 -c<conns> -d10s --latency -s scripts/wrk_pipeline.lua http://127.0.0.1:8081/home <depth>`

Requests/sec by pipeline depth and connection count:

| Pipeline depth \ Connections | 100     | 200     | 400     | 800     | 1000    |
|-----------------------------:|--------:|--------:|--------:|--------:|--------:|
| 1 (keep-alive baseline)      | 531,444 | —       | 566,405 | 575,358 | —       |
| 2                            | 888,676 | 917,438 | 916,677 | 895,321 | 888,301 |
| 4                            | 1,134,229 | 1,188,603 | 1,219,503 | 1,211,835 | 1,220,103 |
| 8                            | 1,219,423 | 1,264,367 | 1,257,014 | 1,298,625 | 1,283,140 |
| 16                           | 1,259,104 | 1,312,601 | 1,368,345 | 1,412,437 | 1,399,043 |

Latency at depth 16 grows with connection count as requests queue:

| Connections | p50     | p99     |
|------------:|--------:|--------:|
| 100         | 890 µs  | 5.26 ms |
| 200         | 1.70 ms | 10.32 ms |
| 400         | 3.62 ms | 24.17 ms |
| 800         | 7.66 ms | 39.91 ms |
| 1000        | 10.52 ms | 45.10 ms |

Batching requests per write raises throughput by about **2.7×** at depth 16
(`-c800`) versus the keep-alive baseline, at the cost of proportionally higher
per-response latency. More connections help modestly once depth is high: the
server admits up to 1024 active connections by default, so `-c1000` is near
capacity and larger values are rejected. Above roughly 1.4M req/s `wrk` and the
loopback interface — not the server — are the bottleneck, so treat these as an
upper bound.

#### Reproducing these runs

`scripts/wrk_benchmark.py` drives `wrk` through all of the sweeps above and
keeps the best requests/sec run of each configuration:

```
# All sweeps, launching the server itself, writing benchmarks/wrk_results.csv
python3 scripts/wrk_benchmark.py --start-server --sweep all \
  --duration 10 --repeats 2 --output benchmarks/wrk_results.csv

# Just the pipelining grid against an already-running server
python3 scripts/wrk_benchmark.py --sweep pipeline --repeats 2
```

It requires `wrk` on `PATH`; `--sweep` also accepts `threads`, `connections`,
and `settings`. The pipelining sweep uses `scripts/wrk_pipeline.lua`.

#### Reference run

The original reference configuration, `wrk -t12 -c400 -d30s --latency`:

| Metric        | Value                                             |
|---------------|---------------------------------------------------|
| Requests      | 23,324,967 in 30.08 s                             |
| Throughput    | 775,443 requests/sec                              |
| Latency       | 600 µs avg, 470 µs p50, 2.80 ms p99, 14.27 ms max |
| Transfer      | 249.18 MB/sec                                     |

These are a single-host snapshot for comparison, not a guarantee. Re-run on the
target machine and server revision before drawing conclusions.

### In-repo benchmark harness

The Python harness implements the measurement contract in
`plans/scaling-plan.md` and records offered rate, completed/successful/failed
requests, status-code distribution, latency percentiles, keep-alive and
new-connection rates, server CPU/RSS/open FDs, queue depth, active workers,
rejected tasks, context switches, and TCP retransmits. Results are appended to
`benchmarks/*.csv`; JSON Lines is written when `--log-file` ends in
`.json`/`.jsonl`.

```
make benchmark         # 1,000 req/s new-connection smoke test
make stress            # 5,000 req/s new-connection, logs stress_results.csv
make benchmark-matrix  # every scenario from plans/scaling-plan.md
```

Selectable scenarios are `new-connection-1000`, `new-connection-5000`,
`keep-alive-5000`, `mixed-paths-5000`, `gzip-500`, `error-paths`, and
`slow-clients`. Custom runs can combine `--rate`, `--duration`, `--path`
(repeatable for a path mix), `--keep-alive`, `--gzip`, and `--expect-status`:

```
python3 scripts/http_benchmark.py --start-server --scenario mixed-paths-5000 \
  --duration 30 --concurrency 32
```

The harness uses only Python's standard library and validates response framing
(Content-Length, chunked, or `Connection: close`) and expected status codes.
Server metrics are exported through `HTTP_SERVER_METRICS_FILE`; access logging
is disabled while the benchmark owns the server (`HTTP_SERVER_ACCESS_LOG=0`).

## Roadmap

| Phase | Focus                                    | Status                                             |
|-------|------------------------------------------|----------------------------------------------------|
| 1     | HTTP/1.1 protocol compliance             | Done — parsing, keep-alive, 400/404/413/501        |
| 2     | Concurrency: thread pool (`pthread`)     | Done — retained as the `USE_EVENT_LOOP=0` path     |
| 3     | Nonblocking `epoll` event loop           | Done — default dispatch                            |
| 4     | Multi-loop scaling and backpressure      | Implemented — `SO_REUSEPORT` loops, capacity admission |
| 5     | OS and deployment tuning                 | Active — `plans/scaling-plan-phase5.md`            |

Not yet implemented: TLS/HTTPS, CGI, reverse proxy, dynamic configuration, and
directory listing. See `plans/` for the specifications behind each phase.

## Learning milestones

1. **Networking foundations** — Phases 1–3: HTTP/TCP framing and I/O models.
2. **Systems programming** — Phase 2 + event loop: concurrency and nonblocking I/O.
3. **Production engineering** — Phases 4–5: scaling limits, backpressure, and
   reproducible measurement.

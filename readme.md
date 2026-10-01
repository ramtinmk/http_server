# C HTTP Server

A small HTTP/1.1 static-file server written in C11, using POSIX sockets,
`epoll`, POSIX threads, and `zlib`. The project doubles as a study of
concurrency models — it grew from a blocking `fork()` server, to a thread pool,
to the nonblocking event loop that is the default today — and as a reproducible
benchmark target: a deterministic file-class corpus and a `wrk` peer comparison
against nginx back the per-class numbers in [Benchmarking](#benchmarking).

## Architecture

- **Dispatch:** nonblocking `epoll` event loop. One event loop runs per online
  CPU core by default (`EL_THREAD_COUNT=0` auto-detects; set a positive value to
  override), and each loop owns a `SO_REUSEPORT` listener so the kernel hashes
  new connections across them.
- **HTTP:** HTTP/1.1 keep-alive with Content-Length framing, request pipelining
  (up to `MAX_PIPELINE_DEPTH`, default 16), and strict message parsing: only
  `HTTP/1.0`/`HTTP/1.1` (else `505`), mandatory `Host` on HTTP/1.1, absolute-form
  targets, and rejection of control characters, obs-fold, duplicate/conflicting
  `Content-Length`, and `Transfer-Encoding` (no request smuggling). Error
  responses cover `400`, `404`, `405`, `406`, `414`, `416`, `417`, `431`, `501`,
  and `505`.
- **Methods:** `GET`/`HEAD` serve a resource, `OPTIONS` returns `204` with
  `Allow`, `POST` returns `405`, and unknown methods return `501`. Request
  bodies are never read; a body-bearing request is answered and closed.
- **Static assets:** `root/home.html` and `root/hello.html` are read once at startup and
  cached in memory, both plain and gzip-precompressed, with strong `ETag`s and
  `Last-Modified`. Conditional requests (`If-None-Match`, `If-Modified-Since`,
  `If-Range`) return `304`; a single `Range` returns `206`/`416` with
  `Accept-Ranges: bytes`, and multiple satisfiable ranges return a bounded
  `206 multipart/byteranges`. Ranges are served uncompressed; full responses
  use `Accept-Encoding` negotiation (gzip) and emit `Vary: Accept-Encoding`.
- **Document root:** every non-alias path is resolved beneath
  `HTTP_SERVER_DOCUMENT_ROOT` (default `.`) by a single safe resolver:
  percent-decode once, normalize `.`/`..` without escaping the root, refuse
  hidden dotfiles by default, and open with `openat2(RESOLVE_BENEATH)` /
  `O_NOFOLLOW` (symlinks denied by default). Directories serve an index file
  (no listing); `Content-Type` comes from a builtin MIME table plus an optional
  `HTTP_SERVER_MIME_TYPES` file. Small files are cached in a byte-budgeted LRU
  (plain + gzip); larger files stream from the descriptor with nonblocking
  `sendfile` (identity only) and still honor conditionals and ranges.
- **TLS termination (Phase 3):** set `HTTP_SERVER_TLS=1` with `tls_cert_file`
  and `tls_key_file` to serve the same document root over HTTPS on a second
  `tls_port` (default 8443). OpenSSL-backed: TLS 1.2 minimum, TLS 1.3 preferred,
  ECDHE-only ciphers, ALPN advertising `http/1.1`, no compression or
  renegotiation, session resumption, and no 0-RTT. The handshake and record I/O
  run nonblocking inside the event loop; a `SIGHUP` reloads the certificate in
  place without dropping connections. The plaintext listener is independent and
  remains the fallback.
- **Resource guards:** bounded active connections, per-connection input cap,
  keep-alive request cap, and read/idle/write timeouts (see
  `include/server_config.h` for every value and its default).
- **Overload behavior:** at connection capacity, each event loop accepts and
  rejects excess sockets in bounded batches. It attempts a complete `503`
  response without blocking; if that cannot be sent immediately, it closes the
  socket promptly with a reset. Listener interest stays enabled, so excess
  connections do not wait for an active slot to free before being rejected.
- **Metrics:** set `HTTP_SERVER_METRICS_FILE=<path>` to emit periodic runtime
  snapshots (connections, response status distribution, timeouts, event-loop
  counters).
- **Memory profiler:** the same snapshots carry a runtime memory sample
  (`rss_kb`, `pss_kb`, `private_dirty_kb`, `vmsize_kb`, `heap_inuse_bytes`,
  `heap_mmap_bytes`, RSS/PSS/heap high-water marks, plus `memory_sample_ok`).
  The reporter thread samples `/proc/self/statm`, `/proc/self/smaps_rollup`, and
  `mallinfo2()` (falling back to `mallinfo()`); the event-loop data path never
  touches procfs or the allocator accounting. `private_dirty_kb` is the
  anonymous resident footprint and a stabler heap proxy than `mallinfo()`'s
  ratcheting in-use figure. See `include/memory_profiler.h`.
- **Allocator arena cap:** glibc arenas are bounded with
  `mallopt(M_ARENA_MAX, 2)` before any thread starts so VmSize does not scale
  with the event-loop count (measured ~600 MB → ~141 MB at 8 loops, RSS
  unchanged). Set `HTTP_SERVER_MALLOC_ARENA_MAX` to override (1 minimizes
  VmSize); a pre-set `MALLOC_ARENA_MAX` is respected. See
  `include/server_config.h`.
- **Configuration:** one validated surface with precedence defaults < config
  file < environment < command line. Connection limits, keep-alive count, the
  read/idle/write/drain timeouts, and logging are runtime-tunable; an invalid
  or unknown key is fatal and names the key. See `docs/env-vars.md` and
  `./bin/http_server --help`.
- **Logging:** leveled JSON records to a file or stderr. Access logging is
  opt-in (`HTTP_SERVER_ACCESS_LOG=1`) and writes through a nonblocking pipe to a
  writer thread, so a slow disk drops records instead of stalling an event
  loop. `SIGHUP` reopens the log file for rotation.
- **Lifecycle:** `SIGTERM`/`SIGINT` stop accepting, finish in-flight responses
  within `HTTP_SERVER_SHUTDOWN_DRAIN_TIMEOUT`, and exit 0; `sd_notify`
  (`Type=notify`) is sent when `NOTIFY_SOCKET` is set.

## Build

Requires a C11 compiler, CMake, `zlib`, and (for TLS) `libssl-dev` / OpenSSL.
CMake fails at configure time when OpenSSL is missing.

```
make
```

The build defaults to `CMAKE_BUILD_TYPE=Release` (`-O2 -DNDEBUG`); pass
`-DCMAKE_BUILD_TYPE=Debug` for an unoptimized build with symbols. Do not
benchmark a Debug build.

The binary is built hardened by default: position-independent executable, full
RELRO, a non-executable stack, `-fstack-protector-strong`,
`-fstack-clash-protection`, and `_FORTIFY_SOURCE=2`. Disable with
`-DENABLE_HARDENING=OFF`; verify with `readelf -h/-l/-d bin/http_server` or
`make phase4-hardening`.

CMake writes executables to `bin/`:

```
./bin/http_server     # server
./bin/run_tests       # C test runner
```

## Run

The server must be launched from the repository root because it opens
`root/home.html` and `root/hello.html` via relative paths. It listens on port `8081`.

```
./bin/http_server
```

Configuration is a single validated surface. The checked-in
`http_server.conf` is compiled in as the default (it ships with
`document_root = root`) and loaded automatically when neither `--config` nor
`HTTP_SERVER_CONFIG` is set. Edit that file for out-of-the-box changes, or
override per run:

```
# point at another config file
HTTP_SERVER_CONFIG=/etc/http_server.conf ./bin/http_server
./bin/http_server --config /etc/http_server.conf

# or directly on the command line / environment
./bin/http_server --port 8081 --max-keepalive-requests 1000
HTTP_SERVER_IDLE_TIMEOUT=5 ./bin/http_server

# serve the same document root over HTTPS on a second port
./bin/http_server --tls 1 --tls-port 8443 \
  --tls-cert-file /etc/http_server/tls/cert.pem \
  --tls-key-file /etc/http_server/tls/key.pem
curl -k https://127.0.0.1:8443/hello

# bind on a privileged port, then drop to an unprivileged account (needs root;
# the document root, log, and certificate must be readable by that account)
sudo ./bin/http_server --port 80 --run-user www-data
```

When `run_user`/`run_group` are set the server binds every listener first, then
irreversibly drops to that identity (all three uids/gids, supplementary groups
cleared) and sets `no_new_privs`/non-dumpable. With neither set it keeps the
invoking identity and applies only the always-on hardening.

`--help` lists every key and its environment variable. An invalid or unknown
key exits non-zero naming the key. With TLS enabled, `SIGHUP` reloads the
certificate in place (see `docs/runbooks/configure-and-reload.md`).

| Path     | Response                     |
|----------|------------------------------|
| `/`      | document-root index, else `root/home.html` (200) |
| `/home`  | `root/home.html` (200)       |
| `/hello` | `root/hello.html` (200)      |
| other    | resolved against the document root: `200`, `403` (hidden/symlink/traversal), or `404` |

Paths that would escape the document root (`..`, encoded `..`, `%00`,
backslashes) are refused with `400`/`403`; hidden dotfiles are `403` unless
`HTTP_SERVER_HIDDEN_FILES=1`.

`OPTIONS` returns `204` with `Allow: GET, HEAD, OPTIONS`; `POST` on any path
returns `405` with the same `Allow`; other methods return `501`. Multi-range
requests return a bounded `206 multipart/byteranges`; a range-set that is
invalid or exceeds the configured bounds is answered with the full `200`.

## Tests

Unit suites do not need a running server:

```
./bin/run_tests ring
ctest --output-on-failure
```

Phase 2 static-serving acceptance (traversal corpus, streaming, cache/fd
budgets, throughput) runs against a temporary document root and writes
`benchmarks/production_phase2_static.json`:

```
make phase2-static
```

Phase 3 TLS acceptance (protocol/cipher/ALPN scan, byte-for-byte bodies over
TLS including a large buffered file, concurrent handshakes, and a `SIGHUP`
certificate reload) generates a throwaway self-signed certificate, starts the
server itself, and writes `benchmarks/production_phase3_tls.json`:

```
make phase3-tls
```

The self-contained TLS E2E suite (OpenSSL client, handshake/ALPN/cipher floor,
cached and 2 MiB streamed bodies, keep-alive, garbage-handshake resilience)
generates its own certificate and starts its own server, so it needs no external
process:

```
./bin/run_tests tls
```

The deterministic corpus generator and the benchmark harness have Python E2E
tests that need no running server:

```
ctest --output-on-failure -R "benchmark_corpus_tests|benchmark_python_tests"
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

A single request-rate number is not a fair comparison: file type selects a
different server code path (in-memory cache, `sendfile` streaming, or TLS record
I/O), so results are reported per class. The workflow is to generate the
deterministic file-class corpus, drive one class at a time, and — for peer
claims — run the same classes against nginx. The taxonomy and reporting rules
live in `docs/benchmarks.md`.

### Deterministic corpus

`make corpus` writes `benchmarks/corpus/` plus a SHA-256 manifest covering every
class (`tiny`, `small`, `medium`, `binary`, `streamed`, `large`, opt-in `huge`)
and `.gz` siblings for compressible classes. Generation is seeded, so a fixed
seed produces byte-identical assets on any host — the basis for comparing
servers and revisions:

```
make corpus
python3 scripts/benchmark_corpus.py --list      # plan, no writes
python3 scripts/benchmark_corpus.py --verify    # files vs manifest
python3 scripts/benchmark_corpus.py --include-huge
```

Drive a single class with the in-repo harness (repeat per class; the harness
sets `HTTP_SERVER_DOCUMENT_ROOT`):

```
python3 scripts/http_benchmark.py --start-server \
    --document-root benchmarks/corpus \
    --path /streamed/movie.bin --keep-alive --rate 2000 --duration 10
```

### Peer comparison against nginx

`scripts/run_nginx_comparison.sh` is one command: it builds nginx and this
server with matched worker counts and `-O2`, starts both (plaintext,
`gzip_static`, TLS), pins servers to CPUs `0-3` and `wrk` to `4-7`, and runs the
identity/gzip/TLS matrix into `benchmarks/nginx_comparison.csv` + `.json`:

```
bash scripts/run_nginx_comparison.sh
```

Recorded on an 8-core i5-1135G7, best of 2; ratio is nginx / ours (below 1.0
means ours is faster), `—` means the class is not part of that mode:

| mode                                | tiny | small | medium | binary | streamed | large |
|-------------------------------------|-----:|------:|-------:|-------:|---------:|------:|
| plaintext identity                  | 1.33 |  1.27 |   1.47 |   2.11 |     1.09 |  0.84 |
| gzip (ours cached vs `gzip_static`) | 1.48 |  1.25 |   1.25 |    —   |      —   |   —   |
| TLS                                 | 0.82 |  1.33 |   1.05 |   0.94 |     1.02 |  0.97 |

nginx leads plaintext cached files (1.3–2.1×; `sendfile` from page cache versus
our heap `write()`), is at parity for streamed/large, and is at parity under TLS
— where neither server can `sendfile`, so nginx's file-serving edge disappears
and ours wins the tiny and binary classes. Per-class RPS, latency, tunables, and
caveats are in `docs/runbooks/compare-against-nginx.md`.

To drive an already-running pair yourself:

```
python3 scripts/compare_servers.py --corpus benchmarks/corpus \
    --target ours=http://127.0.0.1:8081 --target nginx=http://127.0.0.1:8082 \
    --wrk-cpus 4-7 --mode identity --output benchmarks/nginx_comparison.csv
```

### TLS throughput

`make benchmark-tls` runs the keep-alive scenario over HTTPS against a server
started with a throwaway self-signed certificate and records the TLS
`hardware_agnostic_rps` in `benchmarks/tls_benchmark.csv`:

```
make benchmark-tls
```

Pass `--tls` to `scripts/http_benchmark.py` to drive any scenario over HTTPS.

### Peak `wrk` throughput snapshot

A historical single-host snapshot for peak throughput and latency on the tiny
cached assets. It predates the file-class corpus and used a different host than
the comparison above, so treat it as context rather than a cross-server result.
Each configuration was run twice and the higher requests/sec run is reported.

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

### In-repo harness (measurement contract)

The Python harness implements the measurement contract in
`plans/scaling-plan.md` and records offered rate, completed/successful/failed
requests, status-code distribution, latency percentiles, keep-alive and
new-connection rates, server CPU/RSS/open FDs, queue depth, active workers,
rejected tasks, context switches, and TCP retransmits. The `server_memory_*`
columns surface the in-process memory sample (RSS/PSS/Private_Dirty/VMS, heap,
mmap, and high-water marks) from the server snapshot alongside the external
`VmRSS` sample. Results are appended to
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

### OS and deployment tuning

At startup the server prints the host limits it depends on (soft/hard
`RLIMIT_NOFILE`, `net.core.somaxconn`, the effective listen backlog, and
`tcp_rmem`/`tcp_wmem`) and raises the soft FD limit up to the hard limit when
needed. It refuses to start when the effective soft limit is below the
connection budget `MAX_ACTIVE_CONNECTIONS + 64 + 3 × event-loop-count`, so a
misconfigured host fails loudly instead of dropping connections. Set
`HTTP_SERVER_CPU_SET` (a `taskset`-style list such as `0-3`) to pin the whole
server; invalid ranges are fatal.

Check a host before a benchmark and exit non-zero if a requirement is unmet:

```
python3 scripts/http_benchmark.py --check-env                 # ulimit, somaxconn, cores
python3 scripts/http_benchmark.py --check-env --require-governor --expect-cores 8
```

`--check-env` also writes the same host fingerprint (including `somaxconn`,
`effective_backlog`, `tcp_rmem`/`tcp_wmem`, and `cpu_governor`) that every CSV
row carries, plus `server_listen_drops` and accept-error counters aggregated
from the server metrics snapshot.

For reproducible runs, use the pinning wrapper, which splits the online CPUs
between the server and the generator and verifies the environment first:

```
./scripts/run_benchmark_pinned.sh                     # server 0-3, client 4-7 on 8 cores
HPIN_SERVER_CPUS=0-1 HPIN_CLIENT_CPUS=2-3 ./scripts/run_benchmark_pinned.sh
```

`docker-compose.yml` encodes the same envelope for container runs: `nofile`
65536, `net.core.somaxconn=4096`, 2 CPU / 512 MiB limits, and a 512-process
`pids_limit`.

The fixed measurement envelope is `BACKLOG=1024`, `MAX_ACTIVE_CONNECTIONS=1024`,
matrix `--concurrency 32`, and the descriptor ceiling above; compose runs report
envelope-normalized numbers because the CPU/memory cap, not the bare host,
bounds them. Require the performance governor for comparable runs:
`cpupower frequency-set -g performance`, or write `performance` to each
`/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor`.

## Roadmap

| Phase | Focus                                    | Status                                             |
|-------|------------------------------------------|----------------------------------------------------|
| 1     | HTTP/1.1 protocol compliance             | Done — parsing, keep-alive, 400/404/413/501        |
| 2     | Concurrency: thread pool (`pthread`)     | Removed — superseded by the event loop             |
| 3     | Nonblocking `epoll` event loop           | Done — dispatch                                    |
| 4     | Multi-loop scaling and backpressure      | Implemented — one loop per CPU core (`SO_REUSEPORT`), capacity admission |
| 5     | OS and deployment tuning                 | Done — `plans/scaling-plan-phase5.md`              |

The next program takes this to production: HTTP/1.1 + TLS static serving, secure
document-root handling, sandboxing, observability, and a hardened systemd
deployment. See `plans/production-http-server.md`; its Phase 0 (operational
safety and overload), Phase 1 (HTTP/1.1 correctness and caching), Phase 2
(secure document-root serving), and Phase 3 (TLS termination) are complete.
Phase 4 (sandboxing and robustness) has begun: compiler/linker hardening and
privilege drop are done, with the OS sandbox, per-IP resource controls, fuzzing,
and sanitizer builds next. Phase 6 capacity validation has begun
with the deterministic file-class corpus and the nginx comparison — see
`docs/benchmarks.md` and `docs/runbooks/compare-against-nginx.md`.

Still out of scope: HTTP/2, CGI, reverse proxy, dynamic content, and directory
listing. See `plans/` for the specifications behind each phase.

## Learning milestones

1. **Networking foundations** — Phases 1–3: HTTP/TCP framing and I/O models.
2. **Systems programming** — event loop: concurrency and nonblocking I/O.
3. **Production engineering** — Phases 4–5: scaling limits, backpressure, and
   reproducible measurement.

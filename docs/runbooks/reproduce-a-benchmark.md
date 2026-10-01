# Runbook: reproduce a benchmark

Benchmarks are driven by dependency-free Python harnesses and `wrk`. Always run
from the repository root.

## 0. Preflight the host

```bash
python3 scripts/http_benchmark.py --check-env
python3 scripts/http_benchmark.py --check-env --require-governor --expect-cores 8
```

This verifies `ulimit -n`, `net.core.somaxconn`, core count, and optionally the
CPU governor, exiting non-zero with the unmet requirement named.

## 1. Quick smoke

```bash
make benchmark          # 1,000 req/s new-connection, 5 s
make stress             # 5,000 req/s new-connection, appends benchmarks/stress_results.csv
```

## 1b. TLS throughput

```bash
make benchmark-tls      # keep-alive over TLS -> benchmarks/tls_benchmark.csv
```

`--start-server --tls` makes the harness generate a throwaway self-signed
certificate and bind the TLS listener on `--port` (the target uses 8443), so no
operator material is needed. Against an already-running TLS server, drop
`--start-server` and pass `--tls --port <tls_port>`; certificate verification is
disabled for a self-signed test cert unless `--tls-ca-file` is given. The
recorded `mode` column reads `tls-keep-alive` / `tls-new-connection`.

## 2. File-class corpus

Comparisons must run against byte-identical assets, one class at a time. See
`docs/benchmarks.md` for the taxonomy and reporting rules.

```bash
make corpus                                        # -> benchmarks/corpus/
python3 scripts/benchmark_corpus.py --list          # plan, no writes
python3 scripts/benchmark_corpus.py --verify        # files vs manifest
```

Drive one class against the corpus with the harness (repeat per class; add
`--tls` for the transport axis):

```bash
python3 scripts/http_benchmark.py --start-server \
    --document-root benchmarks/corpus \
    --path /streamed/movie.bin --keep-alive --rate 2000 --duration 10 \
    --log-file benchmarks/corpus_matrix.csv
```

## 3. Full matrix (reproducible, pinned)

```bash
./scripts/run_benchmark_pinned.sh
```

It splits the online CPUs between server and generator (defaults: server first
half, client second half), verifies the environment while pinned, then runs
every scenario from `plans/scaling-plan.md` into
`benchmarks/benchmark_matrix.csv`. Override the split when needed:

```bash
HPIN_SERVER_CPUS=0-1 HPIN_CLIENT_CPUS=2-3 ./scripts/run_benchmark_pinned.sh
HPIN_REQUIRE_GOVERNOR=1 ./scripts/run_benchmark_pinned.sh
```

## 4. `wrk` snapshot

```bash
python3 scripts/wrk_benchmark.py --start-server --sweep all \
  --duration 10 --repeats 2 --output benchmarks/wrk_results.csv
```

Needs `wrk` on `PATH`. `--sweep` also accepts `threads`, `connections`, and
`settings`; the pipelining sweep uses `scripts/wrk_pipeline.lua`.

## 5. Memory soak

```bash
make memory-soak        # 5-minute stationary keep-alive run
```

Writes `benchmarks/memory_soak.json` and exits non-zero when RSS/PSS drift over
the final 80% of the run exceeds the tolerance. `heap_*`/`vmsize_kb` are
recorded for attribution but not gated — see `plans/memory-profiler.md`.

## Interpreting a run

- Every CSV/JSON row carries the host fingerprint (`somaxconn`,
  `effective_backlog`, `tcp_rmem`/`tcp_wmem`, `cpu_governor`) and the
  `server_memory_*` columns from the in-process sample.
- The acceptance metric is hardware-agnostic RPS (successful requests per server
  CPU-second), not raw wall-clock throughput.
- Above roughly 1.4M req/s on loopback the generator is the bottleneck; treat
  the top of the pipelining grid as a client-side bound.

Results append to `benchmarks/`; those artifacts are intentionally committed as
evidence.

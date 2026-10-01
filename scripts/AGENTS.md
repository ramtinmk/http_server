# scripts/ Notes

Benchmark and verification tooling. Repo-wide rules: `../AGENTS.md`; how to run
each harness: `../docs/runbooks/reproduce-a-benchmark.md`.

## Conventions

- **Standard library only** for Python. Do not add third-party imports.
- Harnesses assume the repository root as the working directory and spawn
  `./bin/http_server` when `--start-server` is set.
- Result artifacts are committed under `../benchmarks/`; they are evidence, not
  scratch files.
- When you add a metrics column, append it to `CSV_FIELDS` in
  `http_benchmark.py` (trailing additions keep older rows readable) and extend
  the fixture in `../tests/test_http_benchmark.py`.
- `HTTP_SERVER_ACCESS_LOG=0` now genuinely disables access records; the server
  reads it. See `../docs/gotchas.md`.

## Files

- `http_benchmark.py` — primary harness (scenarios, `--check-env` preflight,
  calibration, hardware-agnostic RPS). `--tls` wraps the client sockets in TLS;
  with `--start-server` it generates a throwaway self-signed certificate and
  puts the TLS listener on `--port`, so `make benchmark-tls` needs no operator
  material. Used by several CMake targets.
- `run_benchmark_matrix.sh` / `run_benchmark_pinned.sh` — full matrix; the
  pinned runner splits server/client CPU sets and verifies the environment.
- `saturation_test.py` — below/equal/above-capacity acceptance.
- `phase0_lifecycle_test.py` — invalid-config rejection, runtime keep-alive
  tuning, and `SIGTERM` drain; `benchmarks/production_phase0_lifecycle.json`.
- `phase0_capacity_2x.py` — above-capacity `wrk` run at 2x capacity;
  `benchmarks/production_phase0_2x.json`.
- `phase0_accesslog_test.py` — access-log overhead gate (<5%);
  `benchmarks/production_phase0_accesslog.json`.
- `phase2_static_test.py` — document-root acceptance: traversal/symlink/encoding
  corpus, large-file streaming (partial writes, slow reader), cache/fd budgets,
  and doc-root throughput vs the fixed-path baseline;
  `../benchmarks/production_phase2_static.json`.
- `phase3_tls_test.py` — TLS acceptance: protocol/cipher/ALPN scan, small and
  large bodies over TLS, concurrent handshakes, `SIGHUP` cert reload, and the
  TLS/plaintext throughput ratio; `../benchmarks/production_phase3_tls.json`.
- `startup_failfast_test.py` — missing static asset must fail startup.
- `memory_soak.py` — stationary keep-alive RSS/PSS drift gate.
- `wrk_benchmark.py` / `wrk_pipeline.lua` — raw `wrk` sweeps (needs `wrk`).

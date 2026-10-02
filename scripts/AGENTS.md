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
  material. Used by several CMake targets. `--document-root` points the server
  it starts at an alternate document root (e.g. the generated corpus).
- `benchmark_corpus.py` — deterministic file-class corpus generator plus
  manifest (`make corpus`); taxonomy and reporting rules in
  `../docs/benchmarks.md`.
- `compare_servers.py` — `wrk` peer comparison across corpus classes; takes
  already-running targets (`--target NAME=URL`, `--wrk-cpus`) and tags rows with
  `--mode`. Recipe and fairness rules in
  `../docs/runbooks/compare-against-nginx.md`.
- `run_nginx_comparison.sh` + `nginx_reference.conf` — one-command nginx
  comparison: builds both servers, starts them (plaintext/gzip_static/TLS), and
  drives the identity/gzip/TLS matrix into `../benchmarks/nginx_comparison.*`.
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
- `phase4_hardening_test.py` — Phase 4 hardening acceptance: `readelf` checks
  (PIE, full RELRO, NX stack, stack protector, `_FORTIFY_SOURCE`), `run_user`/
  `run_group` validation, and a real drop to the configured identity;
  `../benchmarks/production_phase4_hardening.json`.
- `phase4_sandbox_resources_test.py` — Phase 4 sandbox/resource E2E acceptance:
  Landlock/seccomp process state and serving, per-IP admission, slowloris
  closure, rlimits, and static systemd-unit checks;
  `../benchmarks/production_phase4_sandbox_resources.json`.
- `fuzz_smoke.py` — runs the opt-in libFuzzer targets over the checked-in seed
  corpus and records `../benchmarks/production_phase4_fuzz.json`.
- `phase4_coverage.py` — runs the focused/server suites under gcov, merges
  runner/server execution data for critical modules, and enforces the checked-in
  `../coverage/phase4_baseline.json`.
- `startup_failfast_test.py` — missing static asset must fail startup.
- `memory_soak.py` — stationary keep-alive RSS/PSS drift gate.
- `wrk_benchmark.py` / `wrk_pipeline.lua` — raw `wrk` sweeps (needs `wrk`).

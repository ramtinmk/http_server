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
- `HTTP_SERVER_ACCESS_LOG` is currently a no-op in the server; setting it in a
  harness does not silence anything. See `../docs/gotchas.md`.

## Files

- `http_benchmark.py` — primary harness (scenarios, `--check-env` preflight,
  calibration, hardware-agnostic RPS). Used by several CMake targets.
- `run_benchmark_matrix.sh` / `run_benchmark_pinned.sh` — full matrix; the
  pinned runner splits server/client CPU sets and verifies the environment.
- `saturation_test.py` — below/equal/above-capacity acceptance.
- `startup_failfast_test.py` — missing static asset must fail startup.
- `memory_soak.py` — stationary keep-alive RSS/PSS drift gate.
- `wrk_benchmark.py` / `wrk_pipeline.lua` — raw `wrk` sweeps (needs `wrk`).

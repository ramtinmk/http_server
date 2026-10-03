# Runbook: verify changes before handing off

Run everything from the repository root.

## 1. Build

```bash
cmake -S . -B build          # required after adding/removing a .c file (source lists are explicit)
make
```

## 2. Fast, server-free suites

```bash
./bin/run_tests ring
python3 tests/test_http_benchmark.py
```

## 3. Server E2E suite

```bash
./bin/http_server &
SERVER_PID=$!
sleep 0.5
./bin/run_tests server
kill "$SERVER_PID" && wait "$SERVER_PID" 2>/dev/null || true
```

The suite must report `VERDICT: ALL PASSED`. It requires the server on
`127.0.0.1:8081`; running from a different directory makes the server fail its
static-asset preflight.

## 4. Full CTest (same prerequisite)

```bash
./bin/http_server & SERVER_PID=$!; sleep 0.5
ctest --output-on-failure
kill "$SERVER_PID" && wait "$SERVER_PID" 2>/dev/null || true
```

## 4b. Phase 0 operational acceptance (config, drain, overload, logging)

These start and stop their own server and need `wrk` on `PATH` for the load
variants:

```bash
make phase0-lifecycle     # invalid config + SIGTERM drain, no wrk
make phase0-2x            # 2x-capacity overload with wrk
make phase0-accesslog     # access-log overhead gate with wrk
make phase2-static        # doc-root traversal corpus + streaming + budgets, no wrk
make phase4-hardening     # ELF hardening + run_user/run_group drop, no wrk
make phase4-sandbox-resources # Landlock/seccomp + resource-control E2E artifact
make phase5-observability # Prometheus + request IDs + SIGHUP reload, no wrk
make phase6-capacity      # plaintext + TLS fixed-rate + 2x overload, needs wrk
make capacity-smoke       # CI throughput gate vs benchmarks/ci_baseline.json
make saturation           # existing Phase 4 overload acceptance
```

Each writes a JSON artifact under `benchmarks/` and exits non-zero on failure.

## 5. Lint (only if clang-tidy is installed)

```bash
make lint
```

`make lint-fix` edits sources in place — review the diff afterwards. There is no
separate typecheck step; `-Wall -Wextra -pedantic` are enabled by
`CMakeLists.txt`, so a clean build is the typecheck.

## What "done" means

- Clean `make` with no new warnings.
- `ring`, `server`, Python benchmark tests, and (when available) lint pass.
- Any plan under `plans/` has its phase checkboxes updated (`AGENTS.md`).
- If you changed behavior or limits, update `docs/` in the same change.

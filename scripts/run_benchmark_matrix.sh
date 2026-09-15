#!/usr/bin/env bash
# Runs every scenario from the measurement contract in docs/scaling-plan.md and
# appends each result to one CSV (or JSONL) for phase-over-phase comparison.
set -u

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(dirname "$script_dir")"
log_file="${1:-$repo_root/benchmarks/benchmark_matrix.csv}"
duration="${2:-5}"

for scenario in \
    new-connection-1000 \
    new-connection-5000 \
    keep-alive-5000 \
    mixed-paths-5000 \
    gzip-500 \
    error-paths \
    slow-clients
do
    echo "=== scenario: $scenario ==="
    python3 "$script_dir/http_benchmark.py" \
        --start-server \
        --scenario "$scenario" \
        --duration "$duration" \
        --concurrency 32 \
        --timeout 30 \
        --log-file "$log_file" || true
done

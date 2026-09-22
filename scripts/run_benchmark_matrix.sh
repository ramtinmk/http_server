#!/usr/bin/env bash
# Runs every scenario from the measurement contract in plans/scaling-plan.md and
# appends each result to one CSV (or JSONL) for phase-over-phase comparison.
set -u

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(dirname "$script_dir")"
log_file="${1:-$repo_root/benchmarks/benchmark_matrix.csv}"
duration="${2:-5}"
min_hardware_agnostic_rps="${3:-1000}"
calibration_seconds="${4:-2}"

# Calibrate once before the matrix. Individual scenarios reuse the per-machine
# cache, so calibration work is not part of any scenario's measured duration.
if ! python3 "$script_dir/http_benchmark.py" \
    --calibrate only \
    --calibrate-force \
    --calibration-seconds "$calibration_seconds"; then
    echo "calibration failed; matrix not run" >&2
    exit 2
fi

matrix_status=0
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
    extra_args=()
    if [ "$min_hardware_agnostic_rps" != "0" ]; then
        extra_args+=(--min-hardware-agnostic-rps "$min_hardware_agnostic_rps")
    fi
    if ! python3 "$script_dir/http_benchmark.py" \
        --start-server \
        --scenario "$scenario" \
        --duration "$duration" \
        --concurrency 32 \
        --timeout 30 \
        --calibrate on \
        --log-file "$log_file" \
        "${extra_args[@]}"; then
        matrix_status=1
    fi
done

# Above-capacity saturation coverage (Phase 4): below/equal/above the configured
# connection limit, with the bounded overload outcome and post-drain checks.
if [ -f "$script_dir/saturation_test.py" ]; then
    echo "=== saturation: capacity 16 ==="
    if ! python3 "$script_dir/saturation_test.py" --capacity 16; then
        matrix_status=1
    fi
fi

exit "$matrix_status"

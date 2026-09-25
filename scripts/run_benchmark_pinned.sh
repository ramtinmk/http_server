#!/usr/bin/env bash
# Phase 5: run the benchmark matrix with the server and the load generator
# pinned to disjoint CPU sets. Pinning removes scheduler migration as a source
# of run-to-run variance and keeps the generator from stealing the server's
# cores.
#
# Core ranges are chosen from the online CPU count so the same command works on
# different hosts; override them explicitly when a host needs a different split:
#
#   HPIN_SERVER_CPUS=0-1 HPIN_CLIENT_CPUS=2-3 ./scripts/run_benchmark_pinned.sh
#
# On an 8-core host this defaults to server=0-3 and client=4-7, leaving no
# core shared. On odd counts the extra core goes to the server, which does the
# heavier lifting. Enumeration is tasklist-order, not topology; pass explicit
# ranges when hyperthread siblings matter.
#
# Usage: run_benchmark_pinned.sh [log_file] [duration] [min_rps] [calib_seconds]
set -u

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if ! command -v taskset >/dev/null 2>&1; then
    echo "taskset(1) is required for pinned runs" >&2
    exit 2
fi

nproc_count="$(nproc 2>/dev/null || echo 0)"
if [ "$nproc_count" -lt 2 ] 2>/dev/null; then
    echo "at least 2 online CPUs are required to pin server and client apart" >&2
    exit 2
fi

server_cores=$(( (nproc_count + 1) / 2 ))
if [ -n "${HPIN_SERVER_CPUS:-}" ]; then
    server_cpus="$HPIN_SERVER_CPUS"
else
    server_cpus="0-$((server_cores - 1))"
fi
if [ -n "${HPIN_CLIENT_CPUS:-}" ]; then
    client_cpus="$HPIN_CLIENT_CPUS"
else
    client_cpus="$server_cores-$((nproc_count - 1))"
fi

if [ "$server_cpus" = "$client_cpus" ]; then
    echo "server and client CPU sets must be disjoint (both '$server_cpus')" >&2
    exit 2
fi

echo "pinned run: server cpus=$server_cpus client cpus=$client_cpus"

# Verify the environment before spending time on the matrix, running the check
# pinned to the client set so --expect-cpus genuinely proves pinning is active.
# Governor checks are opt-in because many development hosts cannot set one.
check_args=()
if [ "${HPIN_REQUIRE_GOVERNOR:-0}" = "1" ]; then
    check_args+=(--require-governor)
fi
if ! taskset -c "$client_cpus" python3 "$script_dir/http_benchmark.py" \
    --check-env \
    --expect-cores "$nproc_count" \
    --expect-cpus "$client_cpus" \
    "${check_args[@]}"; then
    echo "environment check failed; matrix not run" >&2
    exit 2
fi

export BENCH_SERVER_CPUS="$server_cpus"
export BENCH_CLIENT_CPUS="$client_cpus"
exec "$script_dir/run_benchmark_matrix.sh" "$@"

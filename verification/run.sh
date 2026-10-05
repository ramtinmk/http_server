#!/usr/bin/env bash
#
# Reproducible Phase 1 verification entry point (see plans/lean-verify-plan.md).
#
#   1. Builds the Lean proof development. The proof gate is enforced by
#      `set_option warningAsError true` in every Lean module, so an unresolved
#      obligation (`sorry`) or any warning fails the build.
#   2. Emits the Lean model's predictions for the ring-buffer differential.
#   3. Builds and runs the same differential against the real
#      `src/ring_buffer.c`.
#   4. Requires the two tables to be byte-identical.
#
# Run from anywhere: it cd's to the repository root. `make verify` wraps it.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

# elan installs lean/lake into ~/.elan/bin; use it when it is not already on
# PATH. See docs/runbooks/verify-lean-proofs.md.
if ! command -v lake >/dev/null 2>&1 && [ -x "$HOME/.elan/bin/lake" ]; then
    export PATH="$HOME/.elan/bin:$PATH"
fi
if ! command -v lake >/dev/null 2>&1; then
    echo "error: lake not found; see docs/runbooks/verify-lean-proofs.md" >&2
    exit 1
fi

CC="${CC:-cc}"
work="${TMPDIR:-/tmp}/http_server_verify"
mkdir -p "$work"

echo "[verify] building Lean proofs (proof gate: no warnings, no sorry)"
( cd verification/lean && lake build )

echo "[verify] generating Lean model table"
( cd verification/lean && lake exe ring_buffer_diff ) >"$work/lean.tsv"

echo "[verify] building and running the C differential harness"
"$CC" -std=c11 -Wall -Wextra -pedantic -I include \
    verification/c/ring_buffer_diff.c src/ring_buffer.c -o "$work/ring_buffer_diff_c"
"$work/ring_buffer_diff_c" >"$work/c.tsv"

echo "[verify] diffing the Lean model against the C implementation"
if ! diff -u "$work/lean.tsv" "$work/c.tsv"; then
    echo "error: the Lean model and the C implementation disagree" >&2
    exit 1
fi

cases="$(wc -l <"$work/lean.tsv")"
echo "[verify] OK: machine-checked proofs and $cases differential cases agree"

# Runbook: reproduce the Lean verification proofs

**Trigger:** you changed a Lean model/proof under `verification/`, or you want to
re-run the Phase 1 verification artifacts on a fresh host.

**Preconditions:** Linux x86_64, a C compiler (`cc`), and network access to
install the Lean toolchain the first time. The pinned version is
`verification/lean/lean-toolchain` (`leanprover/lean4:v4.34.1`).

The architecture, scope, and boundary are in
[`docs/formal-verification.md`](../formal-verification.md).

## 1. Install the toolchain

The standard path uses [elan](https://github.com/leanprover/elan):

```bash
curl https://raw.githubusercontent.com/leanprover/elan/master/elan-init.sh -sSf \
  | sh -s -- -y
```

`elan` reads `verification/lean/lean-toolchain` and downloads the pinned
toolchain on first use. Confirm it is active:

```bash
cd verification/lean && lake env lean --version   # Lean (version 4.34.1, ...)
```

### Offline / blocked dist-server fallback

If `release.lean-lang.org` (elan's distribution server) is unreachable but
GitHub is reachable, install the pinned toolchain from the GitHub release
archive instead. The `.zip` avoids needing `zstd`/`unzip` on the host (Python's
built-in `zipfile` is enough):

```bash
VER=v4.34.1
DEST="$HOME/.elan/toolchains/leanprover--lean4---$VER"
mkdir -p "$DEST"
curl -L -o /tmp/lean.zip \
  "https://github.com/leanprover/lean4/releases/download/$VER/lean-$VER-linux.zip"
python3 -c "import zipfile; zipfile.ZipFile('/tmp/lean.zip').extractall('/tmp/lean')"
cp -a "/tmp/lean/lean-${VER#v}-linux/." "$DEST/"
chmod +x "$DEST"/bin/*          # the zip does not preserve the exec bit
echo "leanprover/lean4:$VER" > "$HOME/.elan/toolchain"
"$HOME/.elan/bin/elan" default "leanprover/lean4:$VER"
```

## 2. Run the verification target

From the repository root:

```bash
make verify
```

This runs `verification/run.sh`, which:

1. builds the Lean proof development — the proof gate
   (`set_option warningAsError true`) makes any unresolved obligation or warning
   a build error, so a green build means every stated theorem is complete;
2. emits the Lean model's predictions for a bounded family of ring-buffer
   `create → write → read → read` sequences;
3. compiles `verification/c/ring_buffer_diff.c` against the real
   `src/ring_buffer.c` and runs the same sequences;
4. `diff`s the two tables and fails on any disagreement.

A passing run prints:

```
[verify] OK: machine-checked proofs and 4844 differential cases agree
```

## 3. What this does and does not establish

- **Does:** machine-checks the theorems in `verification/lean/` (no `sorry`) and
  shows the model and the real C cursor arithmetic agree on the sampled domain.
- **Does not:** prove the C implementation correct. The connection is a shallow
  embedding plus finite differential testing; see the boundary table and
  assumptions in `docs/formal-verification.md`.
- Independent of the C build: the CMake build, the test suites, and the
  benchmarks are unaffected by `make verify`.

## 4. Troubleshooting

- `lake: command not found` — prepend `$HOME/.elan/bin` to `PATH`, or re-run
  step 1.
- Toolchain download hangs — use the offline fallback in step 1.
- `declaration uses 'sorry'` as an **error** — expected: the proof gate is
  doing its job. Finish the proof before presenting the theorem as established.

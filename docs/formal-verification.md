# Formal Verification (Lean 4)

Architecture and scope for the formal-verification program defined by
[`plans/lean-verify-plan.md`](../plans/lean-verify-plan.md). It records the
selected tooling, which components are in scope, the representation relation
between the Lean models and the C implementation, and — critically — the
boundary between what is **machine-checked**, what is **assumed**, and what is
only **tested**.

> Phase 1 (verification scope and tooling) is complete. No server-wide
> correctness claim is made, and nothing in this document should be read as one.
> The live status is the progress snapshot in the plan.

## Tooling decision

| Option | What it gives | Disposition |
|---|---|---|
| **Lean 4 (core)** | Proof assistant for abstract models, invariants, and refinement | **Selected** as the primary and only proof tool. Pinned to an exact version in `verification/lean/lean-toolchain`. |
| Mathlib | Large mathematics/CS library | Not used: Phase 1 and the ring-buffer proof need only core. Keeping the dependency set empty makes `lake build` small, offline, and reproducible. |
| Experimental Lean C separation logic (e.g. `rdevshp/c_verif_seplogic_lean4`) | Direct separation-logic reasoning about C | Immature and single-maintainer; rejected as a dependency now. Re-evaluate in Phase 6. |
| Frama-C/ACSL (+ Coq), VeriFast, CBMC, VST | Mature C verification | Not Lean; would split the proof environment. Recorded as a fallback for Phase 6 if Lean C-correspondence proves impractical. |
| CompCert | Verified C compiler (Coq) | Out of scope: components are verified, not the compiler. |

**Decision:** Lean 4 core, pinned exactly, with C correspondence staged rather
than assumed.

## Verification boundary

This is the table every claim must be checked against. "Machine-checked" means a
theorem accepted by Lean with no `sorry` (the proof gate turns any warning,
including `sorry`, into a build error).

| Element | Status |
|---|---|
| Ring-buffer consume (`ring_buffer_read` / `ring_buffer_skip` cursor advance) preserves `ring_buffer_validate` | **Machine-checked** — `HttpServer.RingBuffer.readStep_valid` |
| Consume reduces occupancy by exactly `len`; leaves `head`/`capacity` unchanged | **Machine-checked** — `readStep_size`, `readStep_head`, `readStep_capacity` |
| Representation relation (C fields ↔ Lean `State`) is faithful | **Assumed + tested** — a manual shallow embedding, differentially checked on a bounded domain. To be mechanised in Phase 6. |
| C unsigned `size_t` arithmetic equals mathematical `Nat` arithmetic for these operations | **Assumed**, justified by the `MAX_RING_BUFFER_CAPACITY` (16 MiB) bound: no value approaches `SIZE_MAX`, so there is no wraparound to model. |
| Byte storage (`memcpy`, wrap copy), allocation, `ring_buffer_resize`, OOM/`errno` paths | **Out of scope** in Phase 1 |
| HTTP grammar, connection state machine, concurrency, blocking I/O, TLS, OS/libc/compiler | **Out of scope** (later phases; see the plan) |

## Components in scope

Candidate inventory, assessed for formal-verification suitability. Phases in
parentheses are the plan's target phases.

| Component | Character | Suitability |
|---|---|---|
| `src/ring_buffer.c` | Pure bounded buffer + line reader; mutable by-value cursor state | **Selected for the first real target (Phase 3)**; most isolated component with non-trivial invariants. |
| `src/path_resolver.c` | Percent-decode / normalise / policy over byte strings | Strong Phase 4/7 candidate for the string-transformation core (the `openat2` syscall stays outside the model). |
| `src/http_server.c` (parser) | Byte-level HTTP/1.1 grammar | High phase (4); large input language and malformed-input surface. |
| `src/event_loop.c` state machine | Asynchronous, concurrent, per-connection state | High phase (5); concurrency must be isolated as an explicit assumption. |
| `src/config.c` | Validated key/value parsing | Possible later target; mostly pure. |
| `src/client_limits.c`, `src/file_cache.c`, `src/metrics.c` | Shared mutable state, atomics, ownership | Later; concurrency/ownership reasoning. |
| `src/main.c`, `src/tls.c`, `src/log.c`, `src/sandbox.c`, `src/privilege.c` | OS, threads, OpenSSL, I/O | Out of scope for component proofs; treated as assumptions. |

## Architecture

```
verification/
├── lean/                     # Lake project, pinned by lean-toolchain
│   ├── HttpServer/
│   │   └── RingBuffer.lean   # model + machine-checked theorems
│   ├── RingBufferDiff.lean   # Lean-side differential table generator
│   └── HttpServer.lean       # library root (imports every model)
├── c/
│   └── ring_buffer_diff.c    # C-side differential harness (real src/)
└── run.sh                    # build proofs, generate both tables, diff
```

- **Models and proofs** live under `verification/lean/`. Lean core only; the
  project has no dependencies, so `lake build` is offline once the pinned
  toolchain is installed.
- **The proof gate** is `set_option warningAsError true` in every Lean module.
  An unresolved obligation (`sorry`) or any warning fails the build, so a green
  `lake build` is evidence that every theorem is complete.
- **C correspondence is staged.** Phase 1 connects a Lean model to the C code
  with a *shallow embedding* (the C statements are transcribed into a Lean
  function) plus an *exhaustive differential check* on a bounded input domain.
  The embedding fidelity is an explicit assumption, not a theorem; mechanising
  it is Phase 6 work.
- **Conventional tests are unaffected.** Verification never replaces the
  project's tests, fuzzing, sanitizers, or benchmarks (the plan's gates).

### The ring-buffer proof-of-concept

`HttpServer.RingBuffer` models the four cursor fields
(`capacity`, `head`, `tail`, `size`) as a `State`, transcribes
`ring_buffer_validate` as the predicate `Valid`, and transcribes the shared
tail-advance of `ring_buffer_read`/`ring_buffer_skip` as `readStep`:

```c
rb->tail += len;
if (rb->tail >= rb->capacity) { rb->tail -= rb->capacity; }
rb->size -= len;
```

`readStep_valid` proves that consuming `len ≤ size` bytes preserves `Valid`
(the `(tail + size) % capacity = head` invariant in particular). Because
`tail < capacity` and `size ≤ capacity`, `tail + len < 2 * capacity`, so the C
code's single conditional subtraction exactly equals the modular advance the
proof reasons about.

`make verify` additionally compiles `verification/c/ring_buffer_diff.c` against
the real `src/ring_buffer.c` and requires its output for a bounded family of
`create → write → read → read` sequences to be byte-identical to the Lean
model's. This is **finite evidence**, not proof, and is labelled as such.

## Representation relation

| Lean (`HttpServer.RingBuffer`) | C (`src/ring_buffer.c`) |
|---|---|
| `State.capacity` | `RingBuffer.capacity` |
| `State.head` | `RingBuffer.head` |
| `State.tail` | `RingBuffer.tail` |
| `State.size` | `RingBuffer.size` |
| `Valid s` | `ring_buffer_validate(rb) == 1` |
| `readStep s len` | the tail advance in `ring_buffer_read` / `ring_buffer_skip` |

The relation is asserted (as an assumption) to hold after
`ring_buffer_create` + `ring_buffer_write` (no resize) and to be preserved by
`readStep`; the differential harness samples the post-conditions of the real
implementation over the bounded domain and compares them to the model.

## Assumptions

Every assumption is recorded here and must be re-examined in Phase 6.

1. **Shallow-embedding fidelity.** The Lean `readStep`/`Valid` definitions
   transcribe the C statements faithfully. Validated by differential testing on
   `capacity ∈ [1,16]` with all `create → write → read → read` sequences, but not
   proved.
2. **`size_t`/`Nat` correspondence.** All counter values are bounded by
   `MAX_RING_BUFFER_CAPACITY = 16 MiB`, far below `SIZE_MAX`, so C unsigned
   wraparound cannot occur and mathematical `Nat` arithmetic models the C
   arithmetic exactly for the modelled operations.
3. **No resize on the sampled path.** The differential harness stays within
   capacity so `ring_buffer_resize` is never exercised; the resize path is
   outside the model.
4. **Out-of-model C behaviour** (byte copies, allocation, `assert`,
   `errno`-setting) does not affect the modelled counters on the covered paths.

## Limitations and non-claims

- This does not prove the server, the parser, the event loop, or any end-to-end
  property.
- Differential testing is finite evidence; it is not a proof and cannot be
  reported as one.
- Compiler, libc, kernel, and networking behaviour are not modelled.
- Concurrency is not modelled.
- No performance property is (or will be) established with Lean.

## Reproducing

See [`docs/runbooks/verify-lean-proofs.md`](runbooks/verify-lean-proofs.md).
The one-command entry point is `make verify`.

/-
  Differential harness (Lean side).

  Enumerates a bounded family of `create → write → read → read` sequences and
  prints the resulting `(capacity, head, tail, size, valid)` tuple for the Lean
  model. `verification/run.sh` compiles `verification/c/ring_buffer_diff.c`
  against the real `src/ring_buffer.c` and requires the two tables to be
  byte-identical. This validates the representation relation on the sampled
  domain; it is evidence, not a proof, and the boundary is documented in
  `docs/formal-verification.md`.
-/
import HttpServer.RingBuffer

open HttpServer.RingBuffer

-- Any warning (including an unresolved `sorry`) fails the build.
set_option warningAsError true

/-- State after `ring_buffer_create c` followed by `ring_buffer_write s`, with
`s ≤ c` so the exponential-growth resize path is never taken. The C write
advances `head` by `s` and wraps it to `0` at `capacity`; two reads then advance
`tail`. -/
def afterCreateWrite (c s : Nat) : State :=
  { capacity := c, head := s % c, tail := 0, size := s }

/-- Full modelled sequence: create `c`, write `s`, read `r`, read `len`. -/
def modelState (c s r len : Nat) : State :=
  readStep (readStep (afterCreateWrite c s) r) len

def row (s : State) : String :=
  s!"{s.capacity} {s.head} {s.tail} {s.size} {if valid s then 1 else 0}"

/-- `c ∈ [1,16]`, `s ∈ [0,c]`, `r ∈ [0,s]`, `len ∈ [0,s-r]`, in nested-loop
order. The C harness must enumerate exactly this sequence. -/
def cases : List (Nat × Nat × Nat × Nat) :=
  Id.run do
    let mut acc : Array (Nat × Nat × Nat × Nat) := #[]
    for c in List.range 17 do
      if c == 0 then continue
      for s in List.range (c + 1) do
        for r in List.range (s + 1) do
          for len in List.range (s - r + 1) do
            acc := acc.push (c, s, r, len)
    return acc.toList

def main : IO Unit := do
  for (c, s, r, len) in cases do
    IO.println (row (modelState c s r len))

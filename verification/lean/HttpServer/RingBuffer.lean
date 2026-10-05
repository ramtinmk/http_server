/-
  Formal model of the counter arithmetic in `src/ring_buffer.c`.

  Only the fields mutated by the consume/read path are modelled: `capacity`,
  `head`, `tail`, and `size`. Byte storage and `memcpy` are out of scope. The
  representation relation, the assumptions that connect this model to the C
  implementation, and the unmodelled behaviour are documented in
  `docs/formal-verification.md`.

  Proofs use Lean core only (no Mathlib), so the environment is reproducible
  from the pinned toolchain alone.
-/
namespace HttpServer.RingBuffer

-- The proof gate: warnings (including `declaration uses 'sorry'`) are errors, so
-- a proof development with an unresolved obligation cannot build.
set_option warningAsError true

/-- Abstract counter state, mirroring the C `struct RingBuffer` fields that the
read and write cursors use. -/
structure State where
  capacity : Nat
  head     : Nat
  tail     : Nat
  size     : Nat
deriving Repr, DecidableEq

/-- Compile-time ceiling from `src/ring_buffer.c` (`MAX_RING_BUFFER_CAPACITY`). -/
def maxCapacity : Nat := 16 * 1024 * 1024

/-- Transcription of `ring_buffer_validate` for the counter fields. The C
version additionally checks `rb != NULL`, `rb->buffer != NULL`, and
`capacity != 0`; the first two have no analogue in this model and the third is
the `0 < capacity` conjunct below. -/
def Valid (s : State) : Prop :=
  0 < s.capacity ∧
  s.size ≤ s.capacity ∧
  s.head < s.capacity ∧
  s.tail < s.capacity ∧
  (s.tail + s.size) % s.capacity = s.head

instance : DecidablePred Valid := fun s => by unfold Valid; infer_instance

/-- Boolean form of `Valid`, used by `#eval` and the differential harness. -/
def valid (s : State) : Bool := decide (Valid s)

/--
Mirror of the cursor advance used by `ring_buffer_read` and
`ring_buffer_skip`:

    rb->tail += len;
    if (rb->tail >= rb->capacity) { rb->tail -= rb->capacity; }
    rb->size -= len;

The C code subtracts `capacity` at most once; `readStep_valid` is stated only
under `len ≤ size`, which is exactly the guard both call sites enforce and is
what makes a single subtraction sufficient (since `tail < capacity` and
`size ≤ capacity`, `tail + len < 2 * capacity`).
-/
def readStep (s : State) (len : Nat) : State :=
  { capacity := s.capacity
    head := s.head
    tail := if s.tail + len ≥ s.capacity then s.tail + len - s.capacity else s.tail + len
    size := s.size - len }

/-- `(a - cap) % cap = a % cap` whenever `cap ≤ a`. Core-only helper. -/
theorem mod_sub_cap (a cap : Nat) (h : cap ≤ a) : (a - cap) % cap = a % cap := by
  have ha : a = cap + (a - cap) := (Nat.add_sub_of_le h).symm
  rw [ha]
  simp only [Nat.add_sub_cancel_left, Nat.add_mod, Nat.mod_self, Nat.zero_add, Nat.mod_mod]

/-- The consume step preserves the `ring_buffer_validate` invariant and leaves
`head` and `capacity` unchanged; `size` decreases by exactly `len`. This is the
minimal machine-checked C-correspondence proof-of-concept for Phase 1. -/
theorem readStep_valid (s : State) (len : Nat)
    (h : Valid s) (hlen : len ≤ s.size) :
    Valid (readStep s len) := by
  obtain ⟨hcap, hsize, hhead, htail, hmod⟩ := h
  unfold Valid
  simp only [readStep]
  have hsize' : s.size - len ≤ s.capacity := Nat.le_trans (Nat.sub_le _ _) hsize
  have htail' :
      (if s.tail + len ≥ s.capacity then s.tail + len - s.capacity else s.tail + len)
        < s.capacity := by
    split
    · rename_i hge
      omega
    · rename_i hlt
      omega
  have hmod' :
      ((if s.tail + len ≥ s.capacity then s.tail + len - s.capacity else s.tail + len)
          + (s.size - len)) % s.capacity = s.head := by
    split
    · rename_i hge
      have hle : s.capacity ≤ s.tail + s.size := by omega
      have hcalc : s.tail + len - s.capacity + (s.size - len) = s.tail + s.size - s.capacity := by
        omega
      rw [hcalc, mod_sub_cap _ _ hle, hmod]
    · rename_i hlt
      have hcalc : s.tail + len + (s.size - len) = s.tail + s.size := by omega
      rw [hcalc, hmod]
  exact ⟨hcap, hsize', hhead, htail', hmod'⟩

/-- The `head` field is untouched by a consume. -/
theorem readStep_head (s : State) (len : Nat) : (readStep s len).head = s.head := rfl

/-- The `capacity` field is untouched by a consume. -/
theorem readStep_capacity (s : State) (len : Nat) :
    (readStep s len).capacity = s.capacity := rfl

/-- Consuming `len` bytes reduces the occupancy by exactly `len`. -/
theorem readStep_size (s : State) (len : Nat) : (readStep s len).size = s.size - len := rfl

/--
No-wraparound bound: for any valid state within the compile-time ceiling, the
cursor sum that `ring_buffer_validate` compares stays far below `2 ^ 64`. This
is the machine-checked half of the `size_t`↔`Nat` correspondence assumption (the
other half — that the C code performs the same arithmetic — remains a documented
assumption until Phase 6).
-/
theorem cursor_sum_lt_two_pow_64 (s : State) (h : Valid s)
    (hcap : s.capacity ≤ maxCapacity) : s.tail + s.size < 2 ^ 64 := by
  obtain ⟨_, hsize, _, htail, _⟩ := h
  unfold maxCapacity at hcap
  omega

end HttpServer.RingBuffer

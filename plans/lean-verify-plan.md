---
plan_id: formally-verify-c-http-server-with-lean
title: Formal Verification of the C HTTP Server with Lean
category: program
status: draft
owner: Ramtin
created: 2026-09-21
updated: 2026-09-21
related: []
---

# Formal Verification of the C HTTP Server with Lean

## Purpose

Develop a formal model of the HTTP server and progressively establish that selected
parts of the C implementation satisfy formally specified correctness and safety
properties. The project will begin with isolated components and invariants, then
move toward refinement between an abstract server model and the C implementation.
The final scope will be determined by what can be established with explicit,
machine-checked evidence rather than by assuming that the entire server is verified.

## Baseline

The project currently contains a C HTTP server with low-level systems components,
including an HTTP request/response path, connection management, buffering, resource
limits, and an event-driven networking architecture.

The verification baseline must be established before formalization begins.

Evidence to collect:

- Current source tree and project documentation.
- Existing unit and integration tests.
- Existing memory-safety and undefined-behavior checks, if present.
- Existing HTTP parser tests and malformed-input tests.
- Existing ring-buffer tests.
- Existing server state/connection tests.
- Existing benchmark and stress-test results.
- Current compiler and verification-tool versions.
- Current build configuration and supported C language version.

No claim about formal correctness is made at baseline.

## Goals and Non-goals

### Goals

1. Define explicit formal specifications for selected server components.
2. Establish machine-checked invariants for memory-sensitive data structures.
3. Formalize the HTTP parser's relevant correctness properties.
4. Formalize the connection state machine and its legal transitions.
5. Establish correspondence between selected C implementations and their abstract
   specifications where practical.
6. Document assumptions and unverified portions of the implementation.
7. Produce reproducible verification artifacts and proofs.
8. Determine a defensible final statement of exactly what has been formally verified.

### Non-goals

- Proving the entire operating system, C compiler, libc, kernel, or networking stack.
- Proving properties of external clients or servers.
- Proving performance characteristics with Lean.
- Replacing conventional tests, fuzzing, sanitizers, or benchmarks.
- Claiming whole-program formal verification without a complete refinement argument.
- Rewriting the HTTP server in Lean.
- Formally verifying every dependency used by the server.
- Establishing security guarantees that are not represented in the formal model.

## Measurement contract

Quantitative acceptance criteria in this plan concern verification coverage and
reproducibility rather than runtime performance.

### Verification evidence

- **Metric:** Number of formally specified and machine-checked components.
- **Method:** Count components whose stated properties are accepted by the selected
  proof/verification tooling without unresolved proof obligations.
- **Environment:** The documented project toolchain and verification environment.
- **Pass threshold:** Every component marked as formally verified in the final scope
  has zero unresolved proof obligations for its stated properties.
- **Reported fields:** component, specification, verified properties, assumptions,
  proof status, tool versions, unresolved obligations, and evidence location.

### Refinement evidence

- **Metric:** Number of implementation-to-model correspondence claims established.
- **Method:** Each correspondence theorem must be accepted by the proof system.
- **Environment:** Reproducible verification environment documented by the project.
- **Pass threshold:** Every claimed correspondence theorem is machine-checked.
- **Reported fields:** abstract operation, C operation, preconditions, postconditions,
  assumptions, theorem/proof status, and evidence location.

Runtime benchmarks are outside this measurement contract because they do not establish
formal correctness.

## Progress snapshot

- Program status: `[ ]` Not started
- Phase 1 — Verification scope and tooling: `[ ]`
- Phase 2 — Abstract models: `[ ]`
- Phase 3 — Ring buffer verification: `[ ]`
- Phase 4 — HTTP parser verification: `[ ]`
- Phase 5 — Connection state-machine verification: `[ ]`
- Phase 6 — C/model correspondence: `[ ]`
- Phase 7 — Server-level properties: `[ ]`
- Phase 8 — Verification audit and final claims: `[ ]`

## Phases

### formally-verify-c-http-server-with-lean/phase-1: Verification scope and tooling

- **Objective:** Establish the formal-verification methodology and toolchain for the
  project.
- **Complexity:** 3 — Requires selecting a verification architecture and understanding
  the boundary between Lean, C, and any supporting verification tools.
- **Risk:** medium — An unsuitable toolchain could make implementation-level proofs
  impractical.

**Work**

- [ ] Identify the C components that are candidates for formal verification.
- [ ] Define the boundary between Lean proofs, C verification, and conventional tests.
- [ ] Evaluate the available C-to-formal-verification approaches relevant to the project.
- [ ] Select the initial verification tooling.
- [ ] Define the formal assumptions that will be accepted about external components.
- [ ] Create a minimal proof-of-concept using a small C component.
- [ ] Document the selected verification architecture and its limitations.

**Exit criteria**

- [ ] A documented verification architecture exists.
- [ ] A minimal C-to-formal-proof example is machine-checked.
- [ ] The proof environment can be reproduced from project documentation.
- [ ] The boundary between verified code and assumptions is explicitly documented.

---

### formally-verify-c-http-server-with-lean/phase-2: Abstract models

- **Objective:** Define mathematical models for the first server components to be verified.
- **Complexity:** 3 — Requires translating mutable C behavior into precise mathematical
  state and operations.
- **Risk:** medium — An overly detailed model can become difficult to prove, while an
  overly abstract model may fail to say anything useful about the C implementation.

**Work**

- [ ] Define the abstract state representation for the selected components.
- [ ] Define abstract operations and their preconditions.
- [ ] Define postconditions and invariants.
- [ ] Define error and boundary behavior.
- [ ] Prove basic consistency properties of the abstract models.
- [ ] Document assumptions that are intentionally outside the models.

**Exit criteria**

- [ ] Each selected component has an explicit formal state model.
- [ ] Each modeled operation has documented preconditions and postconditions.
- [ ] Core invariants are machine-checked.
- [ ] The model does not depend on undocumented behavior.

---

### formally-verify-c-http-server-with-lean/phase-3: Ring buffer verification

- **Objective:** Establish formal safety and behavioral properties for the ring buffer.
- **Complexity:** 3 — The component has mutable state and boundary conditions but is
  sufficiently isolated for incremental verification.
- **Risk:** medium — Incorrect treatment of wraparound and capacity boundaries can
  invalidate the model.

**Work**

- [ ] Formalize the abstract ring-buffer state.
- [ ] Define valid-state invariants for read and write positions.
- [ ] Define the relationship between stored data, capacity, and occupancy.
- [ ] Formalize read behavior.
- [ ] Formalize write behavior.
- [ ] Formalize empty and full conditions.
- [ ] Prove that valid operations preserve the ring-buffer invariants.
- [ ] Identify the corresponding C operations.
- [ ] Establish implementation-level correspondence where supported by the selected
  verification tooling.

**Exit criteria**

- [ ] All stated ring-buffer invariants are machine-checked.
- [ ] Read and write operations have machine-checked behavioral properties.
- [ ] Boundary cases represented by the specification are covered by proofs.
- [ ] Any unverified C behavior is explicitly listed as an assumption or limitation.

---

### formally-verify-c-http-server-with-lean/phase-4: HTTP parser verification

- **Objective:** Establish formally specified correctness properties for HTTP request
  parsing.
- **Complexity:** 4 — Parsing involves byte-level representation, malformed input,
  multiple protocol states, and potentially substantial correspondence reasoning.
- **Risk:** high — Parser specifications can become significantly more complex than
  expected.

**Work**

- [ ] Define the abstract representation of supported HTTP requests.
- [ ] Define the accepted input language for the verified parser subset.
- [ ] Define malformed-input behavior.
- [ ] Define parser postconditions.
- [ ] Prove properties of the abstract parser.
- [ ] Identify the corresponding C parser operations.
- [ ] Establish C/model correspondence for the selected parser functionality.
- [ ] Connect existing parser tests to the formally specified behavior where practical.
- [ ] Record protocol features intentionally outside the verified subset.

**Exit criteria**

- [ ] The verified HTTP grammar/subset is explicitly documented.
- [ ] Successful parsing has a machine-checked relationship to the abstract request model.
- [ ] Rejected inputs represented by the specification satisfy the specified rejection
  behavior.
- [ ] The verified subset and unverified protocol features are explicitly listed.

---

### formally-verify-c-http-server-with-lean/phase-5: Connection state-machine verification

- **Objective:** Formally specify and verify the legal lifecycle of a server connection.
- **Complexity:** 4 — State transitions interact with parsing, I/O, errors, keep-alive
  behavior, and resource management.
- **Risk:** high — Concurrency and asynchronous I/O can substantially expand the state space.

**Work**

- [ ] Identify the connection states represented by the C implementation.
- [ ] Define the abstract connection state machine.
- [ ] Define legal transitions.
- [ ] Define invalid transitions.
- [ ] Define state invariants.
- [ ] Model relevant I/O and error events.
- [ ] Prove that valid abstract transitions preserve state invariants.
- [ ] Identify the corresponding C state transitions.
- [ ] Establish correspondence for the selected state-machine subset.

**Exit criteria**

- [ ] The modeled connection states and transitions are explicitly defined.
- [ ] State invariants are machine-checked.
- [ ] Every claimed legal C transition has a corresponding modeled transition.
- [ ] Concurrency assumptions are explicitly documented.
- [ ] Unverified asynchronous behavior is explicitly documented.

---

### formally-verify-c-http-server-with-lean/phase-6: C/model correspondence

- **Objective:** Establish machine-checked correspondence between selected C operations
  and their abstract specifications.
- **Complexity:** 5 — This is the central implementation-verification boundary and may
  require substantial reasoning about C memory and execution semantics.
- **Risk:** high — The chosen verification technology may limit the amount of C that can
  be connected to Lean without significant additional infrastructure.

**Work**

- [ ] Select the smallest useful C/model correspondence target.
- [ ] Define the representation relation between C memory/state and abstract state.
- [ ] Define required C preconditions.
- [ ] Prove that the C operation preserves the representation relation.
- [ ] Prove correspondence between C outputs and abstract outputs.
- [ ] Extend correspondence incrementally to additional operations.
- [ ] Record assumptions about compiler behavior, libc, memory allocation, and external
  interfaces.
- [ ] Measure the verification boundary and identify remaining unverified code.

**Exit criteria**

- [ ] At least one non-trivial C operation has a machine-checked correspondence theorem.
- [ ] The representation relation is explicitly documented.
- [ ] All assumptions required by the correspondence proof are documented.
- [ ] No implementation behavior is described as verified unless covered by a checked
  theorem.

---

### formally-verify-c-http-server-with-lean/phase-7: Server-level properties

- **Objective:** Prove selected end-to-end server properties from the verified components.
- **Complexity:** 5 — Requires composing multiple verified components and reasoning across
  parsing, state transitions, buffering, and resource management.
- **Risk:** high — Composition may expose assumptions that were invisible at component level.

**Work**

- [ ] Select server-level properties that can be expressed precisely.
- [ ] Define the abstract server transition system.
- [ ] Connect verified component specifications.
- [ ] Prove preservation of selected server invariants.
- [ ] Prove selected request-to-response properties under explicit assumptions.
- [ ] Analyze interactions with resource limits.
- [ ] Analyze interactions with malformed input.
- [ ] Analyze interactions between connection lifecycle and resource management.
- [ ] Record all server behavior that remains outside the proof boundary.

**Exit criteria**

- [ ] Every claimed server-level property has a machine-checked proof.
- [ ] All required assumptions are explicitly documented.
- [ ] The composition of verified components preserves the stated server invariants.
- [ ] The final verification boundary is explicitly defined.

---

### formally-verify-c-http-server-with-lean/phase-8: Verification audit and final claims

- **Objective:** Produce an auditable statement of what the project formally verifies.
- **Complexity:** 3 — Primarily involves consolidating proofs, assumptions, evidence,
  and limitations.
- **Risk:** low — The main risk is overstating the verification scope.

**Work**

- [ ] Enumerate every formally verified component.
- [ ] Enumerate every verified property.
- [ ] Enumerate every correspondence theorem.
- [ ] Enumerate all assumptions.
- [ ] Enumerate all unverified components and behaviors relevant to the claims.
- [ ] Record exact proof-tool versions and verification environment.
- [ ] Ensure every claimed result has reproducible evidence.
- [ ] Review project documentation for claims that exceed the actual proof boundary.
- [ ] Produce a final verification report.

**Exit criteria**

- [ ] Every formal-verification claim maps to a machine-checked proof or theorem.
- [ ] Every claim identifies its assumptions and scope.
- [ ] Unverified behavior is explicitly documented.
- [ ] Verification artifacts are reproducible in the documented environment.
- [ ] Project documentation does not describe the server as fully verified unless the
  complete implementation is actually covered by the established proofs.

## Gates

The following checks must remain green before progressing or merging verification work:

1. **Conventional test gate**
   - Existing project tests must remain passing.
   - Formal verification must not be used as a replacement for regression testing.

2. **Build gate**
   - The existing project build must remain functional after verification-related changes.

3. **Proof gate**
   - No theorem presented as established may contain unresolved proof obligations.

4. **Scope gate**
   - Every verification claim must identify exactly which implementation and property
     it covers.

5. **Assumption gate**
   - New assumptions about C semantics, memory, concurrency, external libraries, or the
     operating environment must be explicitly documented.

6. **Reproducibility gate**
   - Verification artifacts must be executable in the documented verification environment.

7. **No-overclaim gate**
   - Passing tests, static analysis, fuzzing, or benchmarks must not be presented as
     formal proofs.

## Risks and rollback

| Risk | Mitigation | Rollback |
|---|---|---|
| C/Lean integration becomes impractical | Start with isolated components and validate the toolchain early | Keep formal model independent and limit C correspondence scope |
| Formal model becomes too detailed | Define properties before implementation details | Reduce model to the minimum state required by the property |
| Formal model becomes too abstract | Require every model property to correspond to observable C behavior | Strengthen representation relation |
| Concurrency causes state-space explosion | Begin with sequential components and explicitly isolate concurrency assumptions | Defer concurrent verification to a separate phase |
| Verification tooling becomes a maintenance burden | Keep proofs isolated from production runtime code | Remove verification-only integration without changing server behavior |
| Proof effort expands beyond project scope | Define explicit verification boundaries per phase | Freeze the verified subset and document remaining scope |
| Incorrect claims are made about verification coverage | Maintain a component/property/assumption inventory | Revert documentation claims to the last verified boundary |
| Verification work changes production behavior unintentionally | Keep verification changes independently reversible | Revert production-code modifications while retaining models/proofs |

## Execution order

Execute phases in the following order:

1. `formally-verify-c-http-server-with-lean/phase-1`
2. `formally-verify-c-http-server-with-lean/phase-2`
3. `formally-verify-c-http-server-with-lean/phase-3`
4. `formally-verify-c-http-server-with-lean/phase-4`
5. `formally-verify-c-http-server-with-lean/phase-5`
6. `formally-verify-c-http-server-with-lean/phase-6`
7. `formally-verify-c-http-server-with-lean/phase-7`
8. `formally-verify-c-http-server-with-lean/phase-8`

Phase 4, 5, and 6 may be split into child plans if their implementation scope becomes
too large to satisfy the complexity rules.

Phase 7 must not begin until the relevant component-level properties and implementation
correspondence have been established.

The final project claim must be derived from the actual completed proofs, not from the
original target scope.

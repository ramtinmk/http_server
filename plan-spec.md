# Plan Specification

A project-agnostic standard for writing plans that both humans and agents can
follow. It defines vocabulary, required fields, plan categories, phase
structure, and quality bars. It deliberately contains **no project-specific
facts** (build commands, file paths, test runners); those live in
`AGENTS.md`, `README`, or equivalent project docs.

Version: 1.0

## 1. Scope and precedence

- **This spec** defines *how* a plan is shaped: terms, fields, categories,
  status markers, and acceptance-criteria quality.
- **`AGENTS.md` (or equivalent)** defines *project facts*: commands, layout,
  conventions, dependencies, test entry points.
- **A plan instance** defines *the actual work*.

When they conflict: an explicit instruction in a plan instance overrides this
spec's defaults, but the deviation must be noted in the plan. This spec never
overrides project conventions; it references them.

## 2. Canonical terminology

Use these terms with exactly these meanings. Do not introduce synonyms for them
in a plan.

| Term | Meaning |
| --- | --- |
| **Goal** | The outcome the plan exists to produce. Stated as a result, not an activity. |
| **Non-goal** | An explicitly excluded outcome. Prevents scope creep and wrong expectations. |
| **Baseline** | The measured/observed state before the plan is executed. Include evidence. |
| **Acceptance criterion** | An observable, measurable condition that defines success. |
| **Measurement contract** | The exact method, environment, and reported fields used to evaluate an acceptance criterion (required whenever a criterion is quantitative). |
| **Phase** | A bounded unit of work with its own objective, work items, exit criteria, and complexity. |
| **Work item** | A single actionable task inside a phase. |
| **Exit criterion** | A condition that must be true to consider a phase complete. |
| **Gate** | A required check that must stay green for anything to merge or proceed. |
| **Progress snapshot** | A compressed, current status view derived from phase/work status. |
| **Complexity** | How hard the phase is to implement and reason about (see §7). |
| **Risk** | Likelihood and impact of the phase going wrong (distinct from complexity). |

## 3. Metadata

Every plan begins with a metadata block (YAML front matter is preferred; a
`## Metadata` table is acceptable if the toolchain does not parse front matter).

```yaml
---
plan_id: <stable-kebab-id>          # unique; used for cross-references
title: <human title>
category: <see §4>                  # program | implementation | todo | spike | runbook | design
status: draft | active | blocked | done | abandoned
owner: <person or agent>
created: YYYY-MM-DD
updated: YYYY-MM-DD
related: [<plan_id>, ...]           # parent/child/sibling plans
---
```

Rules:

- `plan_id` must be unique and stable. Reference other plans by `plan_id`, not
  by filename, so renames do not break links.
- Phase identifiers are namespaced by `plan_id` (e.g. `scaling/phase-3`). This
  avoids the common collision where two plans both call unit "Phase 1".
- `status` is updated when the plan changes; `updated` changes with it.

## 4. Plan categories

`category` is not a label; it determines which sections are mandatory and how
much ceremony is expected. Pick exactly one.

| Category | Intent | Mandatory sections | Typical size |
| --- | --- | --- | --- |
| `program` | Multi-phase effort with a program-level outcome | Purpose, Baseline, Goals & Non-goals, Phases, Gates, Acceptance, Risks, Progress snapshot, Execution order | Large |
| `implementation` | One bounded change, or one phase extracted from a program | Purpose, Scope, Steps/Work, Validation, Exit criteria, Risks & Rollback | Small–medium |
| `todo` | An ordered, lightweight actionable list | Objective, Checklist, Done condition | Tiny |
| `spike` | Answer a question under a timebox | Question, Timebox, Method, Findings, Decision | Small |
| `runbook` | An operational procedure | Trigger, Preconditions, Steps, Verification, Rollback | Small |
| `design` | A decision with alternatives | Problem, Constraints, Options, Decision, Consequences | Small–medium |

If a plan does not fit a category, do not invent a category in-place; propose an
addition to this spec first.

## 5. Required sections (common)

Sections below are required when their category lists them. Order is a default,
not a straitjacket, but keep it recognizable.

- **Purpose** — one short paragraph: what and why.
- **Scope** — what is in, what is out, and where the work lives.
- **Baseline** — the observed starting state, with evidence (numbers, links,
  logs). Never assert a baseline without a source.
- **Goals / Non-goals** — results, then explicit exclusions.
- **Measurement contract** — only when an acceptance criterion is quantitative.
  Must name: the metric, the method, the environment, the pass threshold, and
  the reported fields. Warmup, measurement, and drain windows are distinct when
  measuring steady state.
- **Phases** — see §6.
- **Gates** — the correctness/regression checks that must not break. These are
  requirements, not suggestions.
- **Risks & Rollback** — what can go wrong and how to revert safely. Keep each
  limit or change independently reversible where possible.
- **Progress snapshot** — a derived status view (see §8).
- **Execution order** — the sequence to actually execute phases/work items.

## 6. Phases

Each phase is a block with all of the following fields:

```markdown
### <phase-id>: <name>

- **Objective:** <one line; the result this phase produces>
- **Complexity:** <1–5> — <one-line rationale>   # see §7
- **Risk:** <low|medium|high> — <one-line rationale>   # optional but recommended

**Work**

- [ ] <work item>

**Exit criteria**

- [ ] <observable condition>
```

Rules:

- **One objective per phase.** If you need "and", it is probably two phases.
- **Work items are actions; exit criteria are conditions.** Do not mix them.
- **Every phase has at least one exit criterion** and it must be checkable
  without ambiguity.
- **Status markers:** `[ ]` pending, `[~]` in progress/partial, `[x]` done,
  `[!]` blocked. A partial item must state *what remains* in the item text.
- **No phase is "done" based on intent.** It is done only when its exit
  criteria are actually met, including any required verification.

## 7. Cognitive complexity

Complexity measures **how hard the phase is to implement and reason about**, not
how important it is and not how risky it is. Use a 1–5 ordinal scale.

| Score | Anchor |
| --- | --- |
| 1 | Trivial. One file, local change, known pattern, no new interfaces. |
| 2 | Small. A few files in one module, no new concurrency/persistence, familiar approach. |
| 3 | Moderate. Multiple files, one new abstraction or boundary, some design choices, incrementally testable. |
| 4 | High. Cross-module/cross-cutting, new architecture or concurrency model, several interacting invariants, hard to verify locally. |
| 5 | Extreme. System-wide, novel or uncertain design, high fan-out, demands spikes or staged rollout. |

Gating rules:

- A phase rated **4 or 5 must be split** unless a one-line justification is
  recorded and, for 5, a `spike` precedes it.
- A phase rated **3** should be reviewed for splitting.
- If scope changes, re-estimate complexity and update `updated`.
- Do **not** conflate complexity with risk or uncertainty. Record `Risk`
  separately when it matters; a mechanically simple phase can be high-risk
  (e.g. touching a critical path) and a complex phase can be low-risk (e.g.
  isolated, well-tested code).

## 8. Progress and status

- The **progress snapshot** is derived from phase/work status; do not maintain a
  separate conflicting status. Use the marker legend from §6.
- Keep the snapshot near the top so the current state is visible without
  reading the whole plan.
- When a plan spans phases, distinguish `program`-level status (which phases are
  not started / in progress / done) from phase-level work status.
- Blocked items must name the blocker and what unblocks them.

## 9. Acceptance-criteria quality bar

An acceptance criterion is only valid if it is:

- **Observable** — someone other than the author can verify it.
- **Measurable** — a number, a pass/fail, or a named check.
- **Bounded** — scoped in time, rate, count, or environment.
- **Evidence-backed** — the plan names where the evidence comes from.

Reject vague verbs as criteria: *improve*, *faster*, *cleaner*, *better*,
*robust*, *optimize*. Replace them with the specific metric and threshold.

Bad: "Reduce per-request work."
Good: "P99 latency for scenario X is below Y ms over a Z-second steady-state
window with zero unexpected errors; evidence: benchmark report column `p99`."

Quantitative criteria require a measurement contract (§5); if the environment or
method changes between runs, the comparison is invalid.

## 10. Authoring rules

Do:

- State the baseline with evidence before proposing changes.
- Put non-goals next to goals.
- Prefer measurable exit criteria over descriptive ones.
- Keep limits and changes reversible; document rollback.
- Cross-reference related plans by `plan_id`.
- Keep project facts out of plans; link to project docs instead.
- Use the same term for the same concept throughout.

Don't:

- Use plans as a substitute for project conventions.
- Fill a section with prose when no content exists; mark it "not applicable"
  and say why.
- Let checkboxes drift from reality.
- Apply heavy ceremony (`program`) to trivial work; use `todo` instead.
- Bury the current status below long backstory.
- Add a number (like complexity) that no decision depends on.

## 11. Templates

### 11.1 `todo`

```markdown
---
plan_id: <id>
title: <title>
category: todo
status: active
owner: <owner>
created: YYYY-MM-DD
updated: YYYY-MM-DD
related: []
---

# <Title>

**Objective:** <one line>

**Done condition:** <observable end state>

- [ ] <action>
- [ ] <action>
```

### 11.2 `implementation`

```markdown
---
plan_id: <id>
title: <title>
category: implementation
status: active
owner: <owner>
created: YYYY-MM-DD
updated: YYYY-MM-DD
related: [<parent plan_id>]
---

# <Title>

## Purpose
<what and why, in one paragraph>

## Scope
In: <areas/files/modules>
Out: <explicit exclusions>

## Steps
1. [ ] <action>
2. [ ] <action>

## Validation
- [ ] <test/check and how to run it, per project docs>

## Exit criteria
- [ ] <observable condition, with evidence source>

## Risks and rollback
- <risk> → <rollback>
```

### 11.3 `program`

```markdown
---
plan_id: <id>
title: <title>
category: program
status: active
owner: <owner>
created: YYYY-MM-DD
updated: YYYY-MM-DD
related: []
---

# <Title>

## Purpose
## Baseline
## Goals and Non-goals
## Measurement contract
## Progress snapshot
## Phases
### <id>: <name>   (fields per §6)
## Gates
## Risks and rollback
## Execution order
```

## 12. Maintenance

- The spec is versioned. Changing a rule, a category, or the complexity rubric
  requires bumping the version and noting the reason.
- When a plan repeatedly needs to deviate from a rule, update the spec rather
  than accumulating exceptions.
- This spec must be referenced from the project's agent instructions (e.g.
  `AGENTS.md`) so agents load it before authoring or editing plans.

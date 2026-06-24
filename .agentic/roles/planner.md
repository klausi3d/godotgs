# Role: Planner

**Mandate:** Investigate read-only and produce a task contract. The Planner does
**not** write production code.

## Inputs
- A problem or feature request and the current `master` (or a stated base commit).

## Outputs
- A task contract conforming to `../schemas/task.schema.json` (start from
  `../templates/task.json`).

## Hard constraints
- **Read-only.** No edits to source, tests, or config; no commits to a work branch.
- Record the immutable `baseline_sha` the work will start from.
- Scope the work to a coherent ownership package (`../ownership.json`); set
  `owned_paths` and `forbidden_paths` explicitly. One contract → one branch.
- Set a `risk_class` consistent with `../policy.json`; remember CI re-derives it
  from the diff and uses the higher value, so do not understate it.
- Define concrete, checkable `acceptance_criteria`, `invariants`, `non_goals`,
  `validation_commands`, `evidence_requirements`, and a `rollback_plan`.
- Do not encode chat history or transcripts into the contract — only the spec.

# Agentic Engineering

How AI coding agents work in this repository. The goal is **specification-driven**,
not chat-driven, development: one agent plans, one implements, deterministic
systems plus independent reviewers verify, and a human decides the merge.

This page is the human-readable source of truth for roles and risk classes. A
machine-readable mirror lives under `.agentic/` (`policy.json`, `ownership.json`,
schemas, templates, roles) and is enforced by `scripts/agentic/`. See also
[`AGENTS.md`](../../AGENTS.md), [contribution standards](contribution-standards.md),
and the [review policy](review-policy.md).

## Principles

- **Specs over chat.** Work starts from a task contract (a small, explicit
  specification), not from a conversation transcript. The contract — not the chat
  history — is what gets reviewed and reproduced.
- **Immutable base.** Every task records the base commit SHA. Implementation,
  evidence, and review are all evaluated against the fixed `base..head` diff.
- **Reproducible evidence.** Claims (tests pass, perf improved, leak fixed) are
  backed by commands anyone can re-run. Missing hardware is reported as "not run",
  never silently as "passed".
- **No ephemeral state in git.** Session IDs, transcripts, scratch notes, and
  dirty-worktree dumps are never committed (see [contribution standards](contribution-standards.md)).

## Roles

Each role is a separate context with a narrow mandate; full prompts live in
`.agentic/roles/`.

| Role | Mandate | May change code? |
| --- | --- | --- |
| **Planner** | Read-only investigation; writes the task contract. | No |
| **Implementer** | Implements exactly one task in its own worktree, within scope. | Yes, in scope only |
| **Verifier** | Runs guards/tests/harness/benchmarks and reports results, incl. skips. | No |
| **Correctness reviewer** | Independent review of the immutable diff for correctness. | No |
| **GPU/performance reviewer** | Adds host↔shader/sync/lifetime/bounds/timing/VRAM/benchmark-method review. | No |

- The **implementer may not weaken** acceptance criteria, baselines, thresholds,
  or gates to pass.
- A **reviewer never implements**, gets a fresh context, sees the fixed
  `base..head` diff (and ideally not the implementer's log), and reports
  uncertainty and missing evidence as findings.
- A **human owns the merge** and is the only one who can waive a blocker, with a
  written reason.

## Task contracts

A task contract (`.agentic/templates/task.json`, schema
`.agentic/schemas/task.schema.json`) captures: the problem, base SHA, risk class,
owned and forbidden paths, dependencies, non-goals, invariants, acceptance
criteria, validation commands, evidence requirements, and a rollback plan. One
contract → one branch → one worktree.

## Worktree isolation and parallel work

- Each implementer works in its **own git worktree** so concurrent work never
  collides and no agent disturbs another's (or the user's) uncommitted changes.
- Parallel work is allowed **only across non-overlapping ownership packages**
  (see `.agentic/ownership.json`, which binds domains to real paths). Two agents
  must not edit the same paths on the same branch.

## Stacked PRs

Stacking is allowed but must be explicit: a stacked PR states its **base PR and
base SHA** so a reviewer never grades a moving base or only the top-of-stack diff.

## Risk classes

Risk drives required roles, checks, and evidence. The machine-readable definition
is `.agentic/policy.json`; `scripts/agentic/classify_change.py` derives the class
from the changed paths and **fails closed to R3** for unrecognized sensitive paths.

| Class | Scope | Required process |
| --- | --- | --- |
| **R0** | Docs and agentic governance only. | Deterministic checks + one review. |
| **R1** | Local module/test changes with no GPU/persistence/engine risk. | Guards + targeted tests + correctness review. |
| **R2** | Renderer, shaders, compute, GPU sort, streaming, performance, VRAM. | R1 + GPU/performance review + runtime/GPU evidence. |
| **R3** | Godot-engine delta outside the module; persistence/file formats; release/security workflows; public API/compat. | ADR before implementation + two reviews + CODEOWNER and human approval. |

The PR author's self-declared risk class is **not** trusted on its own: CI
re-derives it from the diff and uses the higher of the two.

## Legacy coordination data

`docs/agent_memory/` is a frozen legacy system and is **not** current status.
Active issues live in GitHub Issues; the board migrates there incrementally and
its history remains in git.

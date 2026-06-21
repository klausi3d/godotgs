# Review Policy

How changes are reviewed before a human merges them. Reviews layer four signals;
a change advances only when each required layer is satisfied. See
[agentic engineering](agentic-engineering.md) for roles and risk classes.

## The four layers

1. **Deterministic review.** Compiler/build, guards, targeted tests, contract
   checks, runtime harness, and (for R2+) GPU evidence / benchmarks. These are the
   non-negotiable, reproducible gates — see `docs/reference/build-test-ci.md`.
2. **Independent correctness review.** A reviewer with a fresh context evaluates
   the immutable `base..head` diff for correctness, threading/locking, resource
   lifetime, error/abort paths, back-compat, test quality, and scope creep. The
   reviewer changes no code.
3. **Domain review (R2+).** For renderer/shader/streaming/performance changes, a
   GPU/performance reviewer additionally checks host↔shader contracts, buffer
   bounds and zero paths, device-generation/resource ownership, synchronization
   and async readback, timing honesty, VRAM, and benchmark methodology.
4. **Human disposition.** A human maintainer decides the merge, resolves
   conflicting findings, and is the only party that can waive a blocker — with a
   written reason recorded on the PR.

## Findings

Reviews are emitted as structured findings (`.agentic/schemas/review.schema.json`,
validated by `scripts/agentic/validate_review.py`). Every review records the
`base_sha`/`head_sha` it judged, the tests/evidence it reviewed, and its
**blind spots**. Each finding has a severity:

| Severity | Meaning |
| --- | --- |
| `blocker` | Must be fixed (or waived by a human) before merge. |
| `high` | Strongly expected to be fixed; needs an explicit decision. |
| `medium` / `low` | Should be addressed or consciously deferred. |
| `note` | Informational. |

`blocker` and `high` findings must carry a concrete `required_action`.

## Rules

- **No voting.** Reviewers do not outvote each other. A well-founded blocker stays
  open until it is fixed or a human waives it; a second approval does not clear it.
- **Reviewers do not implement**, and verifiers do not fix — they report.
- A **reconciler** (human or agent) may deduplicate or merge overlapping findings
  but may **not** silently drop or downgrade a blocker.
- The author's self-declared risk class is cross-checked against
  `scripts/agentic/classify_change.py`; the higher class wins and sets the required
  review layers.
- **Untrusted fork code** is never validated on persistent self-hosted runners;
  GPU/Windows validation happens only after a maintainer moves the change onto a
  same-repo branch (see `.github/workflows/README.md`). Any change that relaxes a
  gate or the runner trust boundary must be documented and human-approved.

## Repository enforcement

The branch-protection and required-check settings that back this policy are listed
in [GitHub settings](github-settings.md); they are applied manually by a
maintainer.

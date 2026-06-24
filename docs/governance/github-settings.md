# GitHub Repository Settings

These are the repository rules that back the [review policy](review-policy.md).
They must be configured **manually** by a maintainer in the GitHub UI / rulesets —
nothing in this repo changes them automatically. This page is the checklist of
intended state.

Today `master` has **no required status checks** (documented in
`.github/workflows/README.md`). The settings below close that gap.

## Branch protection / ruleset for `master`

- **Require a pull request before merging** — no direct pushes to `master`.
- **Require at least one approving review from a human maintainer.**
- **Require review from Code Owners** (`.github/CODEOWNERS`). Enable this only
  **after** `.github/CODEOWNERS` (added by a sibling PR in this foundation series)
  has merged; with no owners defined the setting cannot request anyone and the R3
  escalation below stays unenforced.
- **Dismiss stale approvals** when new commits are pushed.
- **Require conversation resolution** — all review threads resolved before merge.
- **Require status checks to pass**, including:
  - `agentic-pr-gate` — the fork-safe, always-on gate's job name (shown in the PR
    UI as `Agentic PR Gate / agentic-pr-gate`). Mark this check required only
    **after** the `agentic_pr_gate.yml` workflow has merged to `master` and reported
    at least once, so PRs are not blocked on a status that cannot report.
- **Require branches to be up to date** before merging (or use the merge queue).
- **Block force pushes** and **block branch deletion** for `master`.

## Risk-class escalation

- **R3 changes** (Godot-engine delta outside the module, persistence/file formats,
  release/security workflows, public API/compat) require **two approvals** and a
  design record (ADR / design-change issue) before implementation. Enforce via a
  CODEOWNERS ownership of the sensitive paths plus a documented reviewer
  expectation; GitHub rulesets cannot encode "risk class" directly, so the
  required second approval for R3 is a maintainer-enforced convention checked at
  review time.

## Runner trust boundary

- Keep the fork guard on every self-hosted job (see
  `.github/workflows/README.md`). Do not add a ruleset or automation that runs
  fork PR code on a persistent self-hosted runner, and never enable a
  `pull_request_target` privileged checkout.

## Emergency bypass

- Repository admins may bypass protection only for a genuine emergency. Any bypass
  must be recorded (PR comment or incident note) with the reason and a follow-up
  to restore normal flow. Bypass is never the routine path.

## Notes

- Required-check names must match the job's reported check name exactly; if the
  gate's workflow/job name changes, update the required-checks list here and in the
  ruleset.

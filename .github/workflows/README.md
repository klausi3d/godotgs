# GitHub Actions Workflows

This directory contains 5 active workflow files.

GitHub's Actions tab can also show historical workflow names from past runs, disabled files, or workflow files that are no longer present in this directory. This README tracks the workflow files currently checked into `.github/workflows/`.

## Active Workflows

| Workflow | File | Purpose | Notes |
| --- | --- | --- | --- |
| Baseline QA Automation | `baseline_qa.yml` | Runs baseline QA and optional compiled-module QA. | Builds the Linux editor once and reuses that artifact for push-only compiled QA. |
| Docs Pages (Versioned) | `docs_pages.yml` | Builds and deploys MkDocs docs with mike versioning to `gh-pages`. | Publishes `latest` from `master/main` and versioned docs from `v*` tags. |
| Gaussian Production Gates | `gaussian_production_gates.yml` | Enforces guard checks, pipeline smoke, runtime validation, the blocking streaming gate, and optional non-blocking benchmark evidence surfaces. | Owns the single Windows build for validation workflows. `streaming-gpu-ci` is the canonical blocking GPU-backed streaming runtime gate; `openworld-proof-dev` and `openworld-proof-weekly` are evidence-only benchmark surfaces. |
| Gaussian Shader Validation | `gaussian_shader_validation.yml` | Validates shader compile matrix and host/shader contract checks. | Focused shader CI gate. |
| Release Builds | `release_builds.yml` | Builds Linux and Windows editors for CI artifacts, nightly prereleases, and optional stable-tag publishes. | Publishes Linux tarballs and Windows zips on the nightly schedule and on `v*` tag pushes. |

## Manual Dispatch Inputs

| Workflow | Input | Options |
| --- | --- | --- |
| `baseline_qa.yml` | `debug_mode` | `true`, `false` |
| `baseline_qa.yml` | `baseline_mode` | `compare`, `update` |
| `gaussian_production_gates.yml` | `run_gpu_lane` | `true`, `false` |
| `gaussian_production_gates.yml` | `run_openworld_proof_dev` | `true`, `false` |
| `gaussian_production_gates.yml` | `run_openworld_proof_weekly` | `true`, `false` |
| `gaussian_production_gates.yml` | `enforce_gpu_readiness` | `true`, `false` |
| `gaussian_production_gates.yml` | `runtime_loops` | integer string |
| `release_builds.yml` | `publish_channel` | `none`, `nightly`, `stable` |
| `release_builds.yml` | `release_tag` | string (`vX.Y.Z` when `publish_channel=stable`) |
| `release_builds.yml` | `release_name` | optional string |
| `release_builds.yml` | `keep_nightlies` | integer string |

## Renderer Release Gate Contract

The renderer/public-alpha evidence policy is maintained in
`docs/reference/renderer_release_gate_manifest.json` and validated with:

```bash
python tests/ci/check_renderer_release_gates.py --mode contract
```

The same contract check is part of `tests/ci/run_module_tests.py --guard-only`,
which is what the Gaussian Production Gates `guards` job runs. The contract check
is deterministic and GPU-free. Public-alpha candidate mode
requires the evidence bundle, a public-alpha channel/tag selector, and a live
issue-label snapshot so P0, P1, and release-blocker issues cannot be bypassed by
release notes or manual workflow choices. The workflow-policy
portion of the checker validates required workflow files and job markers only;
the stronger no-downgrade workflow rules remain documented review policy until
the checker grows a real GitHub Actions behavior parser.

External checks are not automatically renderer release blockers. `qlty check`
is currently documented in the manifest as a deferred, non-blocking external
signal because `master` branch protection has no required status checks and the
repo does not track a qlty configuration/log contract. If branch protection
later requires qlty, update the manifest before treating a qlty result as part
of public-alpha signoff.

## Runner Trust Boundary (fork PRs)

The self-hosted Windows/GPU runners are persistent and must never execute
untrusted code from a fork pull request. All four workflows that use the
self-hosted lane apply the same guard so that fork PRs are skipped on the
self-hosted jobs while same-repo (maintainer) PRs, `push`, `schedule`,
`workflow_dispatch`, and `merge_group` continue to run:

```yaml
if: ${{ github.event_name != 'pull_request' || github.event.pull_request.head.repo.full_name == github.repository }}
```

- `baseline_qa.yml` — `gpu-tests`, `gpu-harness` guarded.
- `release_builds.yml` — `build_windows` guarded.
- `gaussian_production_gates.yml` — `guards` and `module-validation` guarded.
- `gaussian_shader_validation.yml` — `shader-validation` guarded.

`pull_request_target` is not used by any workflow, so fork PRs never get a
privileged checkout. With the self-hosted lanes skipped on fork PRs, the fork-safe
GitHub-hosted blocking signal is the `agentic-pr-gate` check
(`.github/workflows/agentic_pr_gate.yml`), added by a sibling PR in this
agentic-foundation series. **Until that gate workflow is merged, fork PRs touching
only these guarded workflows have no GitHub-hosted blocking gate** — merge the gate
before (or with) relying on this boundary. Note also that `baseline_qa.yml`'s Linux
`cpu-tests` job deliberately skips pull requests
(`if: github.event_name != 'pull_request'`), so it does **not** gate fork PRs.
GPU/Windows validation of an external contribution happens only after a maintainer
moves the change onto a same-repo branch. Any change that relaxes this boundary must
be documented here and in `docs/governance/review-policy.md`.

## Scheduled Triggers

| Workflow | Schedule (UTC) | Behavior |
| --- | --- | --- |
| `baseline_qa.yml` | `30 3 * * *` | Runs in update mode and publishes `qa-regression-baseline` for future compare runs. |
| `gaussian_production_gates.yml` | `30 3 * * 1` | Runs the non-blocking `openworld-proof-weekly` benchmark evidence surface. |
| `release_builds.yml` | `30 2 * * *` | Builds and publishes the nightly prerelease, then prunes older nightly releases and tags. |

## Dependencies

- Python 3.11
- SCons/build toolchain for compiled lanes
- Self-hosted Windows runner attached to this repository with labels `self-hosted`, `Windows`, `X64`, `godotgs`
- Optional GPU evidence label `gpu` for the Windows evidence lane
- Vulkan-capable environment for render-path lanes
- `xvfb` for Linux non-headless rendering checks

## Archived Workflows

Disabled workflows are stored in `../archived-workflows/`.

- `benchmark.yml.disabled`
- `build-engine.yml.disabled`
- `gaussian_pipeline_validation.yml.disabled`
- `test_gaussian_splatting.yml.disabled`
- `test_phase4.yml.disabled`

# Renderer Release Gates

This page defines the release evidence contract for renderer evidence and the
public-alpha gate. The source of truth is
[`renderer_release_gate_manifest.json`](renderer_release_gate_manifest.json);
the checker is `tests/ci/check_renderer_release_gates.py`.

## Predicate

A public alpha is not satisfied by a nightly, a manual workflow choice, a green
advisory report, or a path-filtered subset of renderer checks. Candidate mode
machine-enforces that a candidate declares `release_channel=public-alpha` or
matches a `v*-alpha*` release tag, then passes the manifest predicate:

- an issue snapshot is required, either through `--issues-json` or an embedded
  candidate evidence `issue_snapshot`;
- open `priority:P0`, `priority:P1`, and `release blocker` issues in that
  snapshot must be classified as `blocking`, `accepted_alpha_limitation`, or
  `deferred`;
- if the snapshot is open-only, omitted manifest-tracked `blocking` issues must
  be proven closed through candidate evidence `resolved_manifest_issues`;
- any `accepted_alpha_limitation` must have a user-facing entry in
  `docs/development/known-public-alpha-limitations.md`;
- Linux and Windows artifacts, runtime validation, GPU harness output,
  production evidence, benchmark reports, compatibility sources, docs release
  acceptance, and open-world proof must all be present;
- missing GPU runner availability, manual inputs, path filters, or advisory
  open-world proof cannot downgrade a public-alpha candidate to pass.

## Public Alpha Checklist

The manifest owns the machine-readable checklist. Public-alpha signoff requires
all blocking rows below to be closed by code, evidence, or an explicit accepted
limitation before candidate mode can pass.

| Gate | Classification | Evidence Required |
| --- | --- | --- |
| #351 route/fallback/stage contracts | Blocking | #351 closed or split to non-alpha follow-up; resident, streaming, and serial route failure-injection tests; candidate benchmark rows include `route_uid`, `stage_statuses`, and `fallback_counters`. |
| #352 GPU resource lifetime | Blocking | #352 closed or split to non-alpha follow-up; GPU/RID accounting fails on retained owned resources; required GPU batches report `rid_leak_bytes=0`. |
| #360 public-alpha acceptance gates | Blocking | Candidate evidence bundle passes `--mode candidate`; issue snapshot classifies every open P0/P1/release-blocker issue; known limitations page contains each accepted alpha limitation. |
| #369 opaque `qlty check` | Deferred external signal | `master` branch protection has no required status checks; no repo-owned qlty config is tracked; `qlty check` is documented as non-blocking unless branch protection later requires it. |

Closed umbrella issues #350 and #353 stay closed only because their remaining
release risk is represented by open child blockers and follow-up issues. They
must not be used as evidence that renderer correctness, GPU lifetime, streaming,
or public-alpha acceptance is complete.

## Issue Classifications

`blocking` means the public-alpha candidate fails while the issue remains open.

`accepted_alpha_limitation` means the candidate may pass only when the evidence
bundle points to the canonical known-limitations page and that page explains
user impact, platform/workflow scope, mitigation, and proof that the limitation
does not hide renderer correctness failures.

`deferred` means the issue is explicitly outside the public-alpha gate or is an
external/non-required signal. A deferred issue still needs a rationale and
evidence requirements in the manifest; an unclassified P0/P1 issue is a gate
failure, not an implicit deferral.

## External Status Checks

The local renderer release gate only treats repo-owned checks and required
branch-protection contexts as release blockers. As of 2026-05-21, GitHub branch
protection for `master` reports `required_status_checks=null`, and the repo does
not track a qlty configuration file. Therefore `qlty check` is an advisory
external signal for this gate. A failing, pending, absent, or login-gated qlty
result must not fail `tests/ci/check_renderer_release_gates.py`.

If qlty later becomes branch-protection-required or gets a repo-owned actionable
configuration/log contract, update `external_status_check_policy` in the
manifest and reclassify #369 before cutting a candidate.

## Workflow Policy Scope

The contract checker currently validates that required workflow files exist and
contain the required job markers. It does not yet parse GitHub Actions YAML
deeply enough to prove manual-input bypasses, path-filter bypasses, Linux-only
publishing, unmatched release-file allowances, or open-world advisory-only
behavior. Those no-downgrade rules remain documented review policy in the
manifest until a workflow-behavior parser is added.

## Renderer Evidence

The current foundation profile requires the compositor hazard visual gate and
tracks every `[RequiresGPU]` doctest by a checked snapshot. The snapshot is
deliberately strict: adding, renaming, or deleting a GPU test without updating
the manifest fails contract validation.

`[SceneTree][RequiresGPU]` and `[Importer][RequiresGPU]` tests remain deferred
because the current offscreen GPU harness does not bootstrap those integration
paths. They are still release blockers. Public-alpha signoff and closure of
#350/#360 require either zero deferred tests or explicit waivers in the manifest
with owner, date, issue URL, product risk, mitigation, and a known-limitations
docs path.

## Visual And Benchmark Rules

Visual and benchmark evidence is machine-checkable before it becomes release
evidence:

- required GPU batches must match at least one test, parse doctest summaries,
  avoid timeouts, avoid skips unless explicitly allowed, and report zero RID
  leak bytes;
- blocking visual lanes need committed reference PNGs, nonzero captures, all
  expected references matched, and non-null SSIM/PSNR fields;
- benchmark rows used for public alpha need hardware, driver, OS, binary,
  route/stage, fallback, queue-pressure, timing, and proof metadata;
- timeout-written reports fail;
- `gpu_time_frame_ms` can be positive evidence only when GPU timing is available
  and provenance proves the source. Otherwise the row must explicitly mark GPU
  timing as unavailable and use CPU-observed labels.

The current public performance dashboard remains a non-authoritative snapshot
until a complete candidate evidence bundle exists.

## CI Hook

Run the direct contract check locally:

```bash
python3 tests/ci/check_renderer_release_gates.py --mode contract
```

CI enforces the same contract through the existing module guard entry point:

```bash
python3 tests/ci/run_module_tests.py --guard-only
```

Candidate release validation requires an evidence bundle and an issue snapshot
unless the evidence bundle embeds `issue_snapshot`:

```bash
python3 tests/ci/check_renderer_release_gates.py \
  --mode candidate \
  --candidate-evidence artifacts/public-alpha/evidence_bundle.json \
  --issues-json artifacts/public-alpha/open_issues.json
```

The issue JSON is intended to come from `gh issue list` or the GitHub API with
labels included. When that snapshot is open-only, any manifest-tracked
`blocking` issue omitted from it needs explicit closure proof in
`resolved_manifest_issues` inside the evidence bundle. This PR does not require
live network access for the deterministic contract check.

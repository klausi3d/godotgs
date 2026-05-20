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
- open `priority:P0`, `release blocker`, and alpha-relevant `priority:P1` issues
  in that snapshot must be classified as `blocking`,
  `accepted_alpha_limitation`, or `post_alpha`;
- any `accepted_alpha_limitation` must have a user-facing entry in
  `docs/development/known-public-alpha-limitations.md`;
- Linux and Windows artifacts, runtime validation, GPU harness output,
  production evidence, benchmark reports, compatibility sources, docs release
  acceptance, and open-world proof must all be present;
- missing GPU runner availability, manual inputs, path filters, or advisory
  open-world proof cannot downgrade a public-alpha candidate to pass.

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

Run the contract check locally or in CI:

```bash
python3 tests/ci/check_renderer_release_gates.py --mode contract
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
labels included. This PR does not require live network access for the
deterministic contract check.

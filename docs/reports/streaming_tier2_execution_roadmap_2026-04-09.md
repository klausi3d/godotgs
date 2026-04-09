# Streaming Tier 2 Execution Roadmap

**Date:** 2026-04-09  
**Scope:** `feat/streaming-tier2-refactor` after the validated `Phase 4B` batching cluster  
**Purpose:** Record what Tier 2 streaming work is already complete, what remains, and what must be true before Tier 2 can be called done.

## Current Status

- `Phase 1` Stabilization foundation: complete
- `Phase 2` Hot-path simplification and runtime coverage: complete
- `Phase 3` Contracts and backend planning: complete
- `Phase 4A` Upload and sort ownership simplification: complete and Windows-validated
- `Phase 4B` Current batching/coalescing cluster: complete and Windows-validated
- Separate performance tracking remains out of the main execution path. The earlier `tier_1m` stress miss reproduced as a transient machine-load issue and passed on rerun.
- Known pre-existing editor-side crashes remain separate from Tier 2 streaming work.

## Completed Work

### Phase 1: Stabilization Foundation

Delivered:

- explicit residency state model and scheduler-consistent admission cleanup
- primary-asset residency handling fixes
- residency finalize-state cleanup
- eviction/accounting and atlas publication hardening
- authoritative runtime profile behavior over environment overrides
- explicit residency failure-state surfacing

Key checkpoints:

- `adc39747c3` `streaming: stabilize residency requests and atlas publication`
- `c6cfe994b8` `tests: add dedicated streaming GPU runtime profile`
- `00d25806f2` `tests: make runtime profiles authoritative over env`
- `f36b7286c4` `streaming: surface residency failure states`

### Phase 2: Hot-Path Simplification and Runtime Coverage

Delivered:

- long-walk and residency runtime coverage
- revision-driven asset membership and instance sync caching
- instance upload/rebuild gating on cached state
- route-policy cache and bootstrap retry cleanup
- recovery regression fixes

Key checkpoints:

- `78c6c10063` `tests: expand tier2 streaming runtime coverage`
- `669eb37303` `streaming: cache tier2 instance sync and upload paths`
- `1caefacd1a` `streaming: fix phase2 recovery regressions`

### Phase 3: Contracts and Backend Planning

Delivered:

- shared runtime fidelity policy
- typed world submission contracts
- restore-state preservation for same-owner and cross-world submission flows
- explicit `FrameBackendPlan` routing
- planner/executor stabilization
- zero-splat submission cleanup for routing and shared-content heuristics

Key checkpoints:

- `623571c811` `renderer: centralize runtime fidelity policy`
- `18a76e3ec5` `renderer: introduce world submission contracts`
- `e5e8564cbb` `scene_director: preserve world submission restore state`
- `f9da313524` `renderer: plan frame backend routing`
- `17596a5059` `renderer: stabilize phase3 backend and submission seams`
- `9ec2691332` `scene_director: ignore empty world submissions in shared-content queries`

### Phase 4A: Upload and Sort Ownership Simplification

Delivered:

- semaphore-agnostic pack-queue ownership
- explicit worker-vs-sync ownership boundary coverage
- Windows test-plumbing fixes required to validate the path
- registered Windows-reachable ownership tests
- centralized instance-count readback ownership for the sorter path

Key checkpoints:

- `bdd06116a6` `streaming: make pack queue ownership semaphore-agnostic`
- `441f81335c` `tests: cover pack queue ownership boundaries`
- `a7a1cba43d` `fix: resolve five MSVC build failures blocking Windows test validation`
- `ac1f085601` `fix: world submission respects route_policy instead of forcing resident`
- `9404f50388` `tests: register pack ownership boundary coverage`
- `1b1125c235` `sort: centralize instance-count readback ownership`

Validation status:

- Windows `tests=yes` build: passed
- targeted ownership doctests: passed
- `streaming-gpu-ci`: passed

### Phase 4B: Current Batching / Coalescing Cluster

Delivered:

- `TESTS_ENABLED` plumbing for doctest targets
- chunk-meta upload planning:
  - compact dirty spans stay incremental
  - fragmented or sufficiently large churn escalates to one full upload
- pack snapshot scratch-buffer reuse
- contiguous upload-slice coalescing for full-slot front-of-queue jobs within budget
- registered tests for incremental/full chunk-meta planning and upload coalescing behavior

Key checkpoints:

- `9f24e19e6b` `tests: define TESTS_ENABLED for doctest targets`
- `443b8d31d6` `streaming: coalesce fragmented chunk meta uploads`
- `e820ad3b6b` `streaming: reuse pack snapshot scratch buffers`
- `13ee319f46` `streaming: coalesce contiguous upload slices`

Validation status:

- Windows `tests=yes` build: passed
- Windows targeted batching/ownership doctests: passed
- targeted doctests:
  - `Sync pack rescue does not steal worker-owned pack jobs`
  - `Clearing instance pipeline inputs resets readback ownership state`
  - `Chunk meta upload planner keeps compact dirty spans incremental`
  - `Chunk meta upload planner escalates fragmented churn to a full upload`
  - `Upload coalescing planner batches contiguous full-slot uploads`
  - `Upload coalescing planner stops at partial or noncontiguous uploads`
- module lane: passed apart from the unchanged pre-existing editor crash
- `streaming-gpu-ci`: passed, including `GPU Streaming Stress`

## Remaining Roadmap

### Phase 4C: Benchmark and Operations Closeout

Status: next active work

Goal:

- close the remaining Tier 2 production-readiness gaps that are about operability and confidence, not architecture

Rules for this phase:

- do not reopen the architectural seams already stabilized in `Phase 1` through `Phase 4B`
- keep `streaming-gpu-ci` as the single canonical blocking GPU-backed streaming gate
- treat performance anomalies as separate investigations unless a `Phase 4C` change clearly causes them
- land this phase in narrow closeout checkpoints, not one large “ops cleanup” patch

#### Phase 4C.1: Benchmark and Gate Closeout

Goal:

- make the benchmark and CI surfaces honest about which lanes are synthetic, which lanes are chunked, and which surfaces are actually blocking

Why this comes first:

- the benchmark and gate surfaces are the public and CI-facing proof that the refactor is production-ready
- if those surfaces remain ambiguous, the rest of the Tier 2 closeout work is hard to trust

Primary files:

- `tests/fixtures/benchmark_asset_manifest.json`
- `tests/runtime/prepare_synthetic_assets.py`
- `tests/runtime/run_benchmark.py`
- `tests/runtime/run_benchmark_suite.py`
- `tests/examples/godot/test_project/scenes/benchmark_suite/*`
- `docs/testing/benchmark-suite.md`
- `tests/runtime/README.md`
- `.github/workflows/gaussian_production_gates.yml`
- `.github/workflows/README.md`

Required work:

- audit every streaming-relevant benchmark lane and classify it explicitly:
  - real chunked scene
  - deterministic synthetic scene
  - intentionally lightweight smoke lane
- remove any implicit “looks like streaming coverage but actually falls back to `test_splats.ply`” ambiguity for the lanes used to justify streaming claims:
  - `streaming_corridor`
  - `city_flyover`
  - `long_soak`
  - `unified_composite`
- keep the benchmark runners aligned so the lane roster, profile selection, and asset expectations do not drift between:
  - `tests/runtime/run_benchmark.py`
  - `tests/runtime/run_benchmark_suite.py`
- make the docs say exactly which profile is canonical for:
  - blocking GPU-backed streaming validation
  - broader release-ready runtime validation
  - benchmark evidence collection
- keep `Gaussian Production Gates` aligned with that policy and avoid adding a second competing streaming gate

Expected output:

- an updated benchmark asset manifest and/or lane configuration that clearly marks which streaming lanes are real chunked evidence
- benchmark docs that distinguish published evidence, suite-only lanes, and synthetic support lanes without ambiguity
- one canonical description of the blocking streaming gate across runtime docs and workflow docs

Validation:

```bash
python3 tests/runtime/run_runtime_validation.py --list-profiles
python3 tests/runtime/run_runtime_validation.py --profile streaming-gpu-ci --skip-cpp --fail-on-skip
python3 tests/runtime/run_benchmark.py --profile performance --fail-fast
```

Exit criteria:

- every streaming-relevant benchmark lane has an explicit asset/evidence classification
- there is no hidden manifest fallback that can make a streaming lane look more representative than it is
- `streaming-gpu-ci` remains the canonical blocking streaming gate in both docs and workflow surfaces

#### Phase 4C.2: Monitor and Telemetry Closeout

Goal:

- make the streaming monitor surface strong enough to explain sustained pressure, queue health, and multi-renderer behavior without reading code

Why this is separate:

- the current monitor surface is broad, but it is still biased toward instantaneous values and “most recently active renderer” behavior
- Tier 2 needs an operable monitor contract, not just a large monitor list

Primary files:

- `modules/gaussian_splatting/core/performance_monitors.cpp`
- `modules/gaussian_splatting/core/performance_monitors.h`
- renderer-side streaming snapshot producers consumed by `get_monitor_streaming_snapshot()`
- relevant test files under `modules/gaussian_splatting/tests/`

Required work:

- add sustained-frame counters for the conditions that matter operationally:
  - queue pressure active for N consecutive frames
  - upload frame cap hit for N consecutive frames
  - upload bandwidth cap hit for N consecutive frames
  - chunk load cap hit for N consecutive frames
  - VRAM chunk cap hit for N consecutive frames
  - runtime-not-ready or invalid-buffer conditions that persist beyond one transient frame
- define and document the intended multi-renderer behavior for streaming monitors:
  - active renderer only
  - aggregate across registered renderers
  - aggregate for some monitors and active-only for others
- add focused test coverage for:
  - sustained-counter rollover and reset
  - multi-renderer aggregation or selection semantics
  - monitor readiness behavior when streaming data is absent vs present
- make sure the monitor names and semantics line up with the playbook and CI expectations

Expected output:

- new sustained counters or equivalent analytics surfaces for streaming pressure conditions
- clarified multi-renderer monitor semantics
- tests that lock those semantics down

Validation:

```bash
python3 tests/ci/run_module_tests.py --guard-only
python3 tests/runtime/run_runtime_validation.py --profile streaming-gpu-ci --skip-cpp --fail-on-skip
```

Exit criteria:

- an operator can tell from monitors whether a failure mode is transient or sustained
- multi-renderer behavior is deliberate and documented, not incidental
- the monitor surface can explain queue pressure, cap hits, and readiness failures without log spelunking

#### Phase 4C.3: Streaming Production Playbook and Final Closeout

Goal:

- make Tier 2 operable by someone who did not implement the refactor

Primary files:

- `docs/operations/streaming-production-playbook.md`
- `tests/runtime/README.md`
- `docs/testing/benchmark-suite.md`
- `.github/workflows/README.md`
- `docs/reports/streaming_tier2_execution_roadmap_2026-04-09.md`

Required work:

- add a production playbook that answers:
  - what to run locally before asking for review
  - what the blocking CI surfaces are
  - how to interpret the key streaming monitors
  - how to distinguish correctness regressions from transient perf noise
  - what to check first when `World Streaming Gate`, `Streaming Residency API`, or `GPU Streaming Stress` fails
- include the canonical validation commands for:
  - Windows `tests=yes` build
  - targeted ownership/batching doctests
  - module lane
  - `streaming-gpu-ci`
  - benchmark collection
- document the benchmark evidence policy:
  - which lanes are representative
  - which lanes are synthetic support scenes
  - what is acceptable evidence for a production-readiness claim
- finish the Tier 2 closeout note and remaining issue list

Expected output:

- `docs/operations/streaming-production-playbook.md`
- aligned docs across runtime, workflow, benchmark, and roadmap surfaces
- a final Tier 2 closeout checklist that a reviewer can execute without tribal knowledge

Validation:

- docs link cleanly from the reports/testing/operations surfaces
- the playbook commands match the actual workflow and runtime entrypoints
- the existing validated `Phase 4A` / `Phase 4B` regression set still passes

Exit criteria:

- a contributor can run the Tier 2 streaming validation path from docs alone
- the playbook explains the monitor surface and failure-triage flow in practical terms
- Tier 2 no longer depends on private context from the refactor branch history

#### Phase 4C Non-Goals

The following remain out of scope for this closeout phase:

- reopening sorter internals beyond the stabilized ownership/readback seams
- changing the canonical runtime gate from `streaming-gpu-ci` to a new profile
- broad renderer architecture refactors
- folding pre-existing editor crashes into the Tier 2 streaming execution path
- treating one-off perf noise as a reason to reopen already-validated functional seams

### Tier 2 Closeout

Tier 2 should only be called complete when all of the following are true:

- the validated `Phase 4A` and `Phase 4B` ownership/batching regression set stays green as `Phase 4C` lands
- `streaming-gpu-ci` stays green as the canonical blocking streaming gate
- the benchmark and runtime docs clearly distinguish chunked evidence from synthetic support lanes
- the benchmark manifest and lane policy no longer overstate streaming coverage
- sustained monitor counters and multi-renderer semantics are implemented and tested
- `docs/operations/streaming-production-playbook.md` is merged and linked from the docs surfaces that need it
- the remaining benchmark/ops closeout work is merged without reopening Tier 1-4B architecture
- pre-existing non-streaming crashes are either unchanged and tracked separately, or fixed in separate work

## Separate From The Main Tier 2 Path

These should stay out of the execution-critical refactor path unless they become reproducible regressions caused by Tier 2 work:

- pre-existing `[Editor]` importer inspector teardown crash
- unrelated editor / SceneTree crash behavior outside the streaming path
- transient machine-load performance anomalies unless they reproduce as real regressions

## Recommended Next Order

1. Land `Phase 4C.1` benchmark and gate closeout first.
2. Keep the current `Phase 4A` / `Phase 4B` validation set as the regression gate while `Phase 4C.1` lands.
3. Land `Phase 4C.2` monitor and telemetry closeout second.
4. Land `Phase 4C.3` production playbook and final Tier 2 closeout last.
5. Write the Tier 2 completion note and remaining non-Tier-2 issue list once all `Phase 4C` checkpoints are merged.

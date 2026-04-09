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

Scope:

- replace synthetic streaming benchmark lanes with real chunked scenes where still missing
- keep one explicit streaming GPU gate as the canonical blocking runtime gate
- add sustained-frame counters and better multi-renderer aggregation for monitors
- write the streaming production playbook / tuning guide

Expected outputs:

- updated benchmark scenes or benchmark manifest
- monitor additions with test coverage
- `streaming-production-playbook.md`
- clear “streaming production-ready” validation checklist

### Tier 2 Closeout

Tier 2 should only be called complete when all of the following are true:

- the validated `Phase 4B` batching cluster stays green as `Phase 4C` lands
- `streaming-gpu-ci` stays green
- targeted registered doctests cover the ownership and batching planners
- the remaining benchmark/ops closeout work is merged
- pre-existing non-streaming crashes are either unchanged and tracked separately, or fixed in separate work

## Separate From The Main Tier 2 Path

These should stay out of the execution-critical refactor path unless they become reproducible regressions caused by Tier 2 work:

- pre-existing `[Editor]` importer inspector teardown crash
- unrelated editor / SceneTree crash behavior outside the streaming path
- transient machine-load performance anomalies unless they reproduce as real regressions

## Recommended Next Order

1. Start `Phase 4C` benchmark and operations closeout.
2. Keep the current `Phase 4B` validation set as the regression gate while `Phase 4C` lands.
3. Write the Tier 2 closeout note and remaining issue list once `Phase 4C` is merged.

# Streaming Remaining Architecture Issue Backlog

Use this file as the direct issue-opening source for the remaining streaming architecture work after the current `Phase 4C.1` proof backlog.

This list is intentionally narrow and ordered. The first two issues are the immediate proof-path blockers. The rest are the structural scaling issues that decide whether the streaming architecture is actually fit for large open-world scenes.

## 1. Streaming: Make World Submissions First-Class Instance-Pipeline Assets

Priority: `P0`

Depends on: none

Unblocks:

- `open_world_corridor_proof` visible output
- proof-lane reclassification
- removal of synthetic fallback dependence for `gsplatworld` scenes

Problem:

`gsplatworld` submissions still do not enter the renderer through the same instance/asset-registration contract as normal instances. The current path applies raw world submission state directly to the renderer, then relies on synthetic fallback instances to bring up the streaming cull/sort/raster contracts. That is why the proof lane keeps finding seams around world submissions instead of behaving like a first-class streamed scene.

Scope:

- give active world submissions an explicit instance-pipeline asset snapshot path
- stop relying on `world->instances` as the only source of streaming asset registration
- keep the world-submission path compatible with the shared cull/sort/raster pipeline
- preserve the zero-splat protections already landed
- add focused regression coverage for a streamed world submission reaching the instance pipeline without synthetic shims

Primary files:

- `modules/gaussian_splatting/core/gaussian_splat_scene_director.cpp`
- `modules/gaussian_splatting/renderer/render_streaming_orchestrator.cpp`
- `modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp`
- `modules/gaussian_splatting/tests/test_scene_director_submission_scaffolding.h`
- `modules/gaussian_splatting/tests/test_renderer_pipeline.h`

Non-goals:

- replacing the whole SceneDirector contract model
- broad renderer refactors outside the world-submission seam
- proof-metric changes

Validation:

- `open_world_corridor_proof` reaches nonzero `uploaded_splats`
- `open_world_corridor_proof` reaches nonzero `visible_splats`
- world-submission regression tests stay green

Done when:

- a renderable world submission reaches the streaming instance pipeline without depending on a synthetic-primary fallback workaround
- the proof lane no longer dies at the world-submission / instance-pipeline seam

## 2. Streaming: Make Readiness And Progress Track Atlas-Published GPU Truth

Priority: `P0`

Depends on:

- `Streaming: make world submissions first-class instance-pipeline assets`

Unblocks:

- trustworthy proof telemetry
- reliable stall diagnosis
- correct large-world readiness gating

Problem:

The streaming system currently reports CPU-side chunk load progress ahead of atlas publication and ahead of the renderer's real cull-ready state. That creates false progress and makes proof runs much harder to interpret, because `loaded_chunks` can advance while `ChunkMetaGPU.splat_count` is still zero and the renderer still has no usable streaming output.

Scope:

- separate CPU-loaded, upload-pending, atlas-published, and render-ready chunk states in telemetry and readiness checks
- stop treating CPU-side `is_loaded` alone as proof of draw-readiness
- align proof metrics and diagnostics with the published atlas/cull-consumable state
- tighten the `open_world` proof surface so false progress cannot pass as success

Primary files:

- `modules/gaussian_splatting/core/gaussian_streaming.cpp`
- `modules/gaussian_splatting/core/streaming_global_atlas_registry.cpp`
- `modules/gaussian_splatting/core/streaming_visibility_controller.cpp`
- `modules/gaussian_splatting/core/performance_monitors.cpp`
- `tests/runtime/run_benchmark.py`
- `tests/examples/godot/test_project/scenes/benchmark_suite/benchmark_suite_lane.gd`

Non-goals:

- retuning VRAM budgets or upload-worker counts
- changing benchmark lane content
- rewriting the GPU cull shader unless the state trace proves it is wrong

Validation:

- proof reports distinguish CPU-loaded chunks from atlas-published resident chunks
- renderer readiness failures name the exact missing published state
- a chunk reported as render-ready has nonzero published `ChunkMetaGPU.splat_count`

Done when:

- proof and runtime telemetry cannot claim chunk readiness before atlas publication
- the next large-world proof failure, if any, points to one concrete state transition instead of ambiguous "loaded but invisible" output

## 3. Streaming: Size Cull And Sort Capacity From Real Working Set, Not Atlas Upper Bound

Priority: `P1`

Depends on:

- `Streaming: make readiness and progress track atlas-published GPU truth`

Unblocks:

- more credible large-world working-set budgeting
- lower allocator churn in large streamed scenes
- tighter render-capacity scaling for multi-instance scenes

Problem:

The streaming instance path still sizes visible/cull/sort buffers from broad upper bounds such as atlas capacity and `instance_count * dispatch_chunk_count * max_chunk_splats`, then clamps against sorter capacity. That is safe, but it means the render path can over-allocate or rebuild around worst-case theoretical coverage instead of the current resident/visible working set.

Scope:

- split "possible maximum" from "current working-set capacity" in the instance pipeline
- stop using atlas-capacity-derived splat counts as the default visible budget when resident/visible evidence is available
- keep sorter rebuilds tied to actual working-set requirements rather than atlas-scale upper bounds
- add regression coverage for capacity planning under streamed-world conditions

Primary files:

- `modules/gaussian_splatting/renderer/render_streaming_orchestrator.cpp`
- `modules/gaussian_splatting/renderer/gpu_sorting_config.h`
- `modules/gaussian_splatting/interfaces/gpu_sorting_pipeline.cpp`
- `modules/gaussian_splatting/tests/test_renderer_pipeline.h`

Non-goals:

- changing the visual quality target
- making hundreds of millions simultaneously visible
- removing the safety clamp against sorter capacity

Validation:

- streamed-world buffer sizing matches resident/visible demand, not atlas capacity alone
- sorter rebuilds no longer grow because of atlas-scale theoretical maxima without matching visible demand

Done when:

- the instance pipeline can explain its working-set capacity from current streamed demand
- large-world proof runs do not allocate or rebuild from atlas-capacity upper bounds by default

## 4. Streaming: Replace O(Total Chunks) Visibility Discovery With A Spatially Bounded Index

Priority: `P1`

Depends on:

- `Streaming: make world submissions first-class instance-pipeline assets`

Unblocks:

- large-world traversal scaling
- stable CPU cost as world chunk counts grow
- honest hundred-million-total open-world claims

Problem:

Visibility discovery still linearly scans all chunks every frame. That means CPU cost scales with total chunk count, not with nearby world cells or the current traversal neighborhood. This is the biggest structural mismatch with the open-world goal after the proof-path seams are fixed.

Scope:

- introduce a spatial index or cell-bounded chunk discovery path for visibility and scheduling
- preserve current chunk bounds and frustum semantics
- make zero-visible recovery bounded and local instead of "show everything"
- keep the scheduler and proof metrics compatible with the new discovery model

Primary files:

- `modules/gaussian_splatting/core/streaming_visibility_controller.cpp`
- `modules/gaussian_splatting/core/gaussian_streaming.cpp`
- `modules/gaussian_splatting/core/streaming_runtime_state.h`
- `tests/runtime/test_world_streaming_gate.gd`
- `tests/runtime/test_gpu_streaming_eviction_churn_probe.gd`

Non-goals:

- changing chunk format
- changing the renderer-side cull shader
- broad camera-system rewrites

Validation:

- per-frame visibility discovery no longer iterates all chunks in the large-world path
- zero-visible recovery stays local and bounded
- large-world proof lanes show stable traversal CPU cost as total chunk count grows

Done when:

- visibility discovery scales with a bounded nearby region or spatial query rather than the full chunk set

## 5. Streaming: Move Large-World Chunk Payloads Out Of Full In-Memory GaussianData

Priority: `P1`

Depends on:

- `Streaming: replace O(total chunks) visibility discovery with a spatially bounded index`

Unblocks:

- true large-world total-scene scaling
- honest out-of-core streaming claims
- lower process memory pressure for hundred-million-total scenes

Problem:

The current streaming system still keeps full source `GaussianData` in memory and packs chunk uploads from that in-memory payload. That bounds the resident VRAM working set, but it does not solve large total-scene memory scaling for open-world scenes.

Scope:

- load chunk payloads from staged world/chunk storage instead of requiring the full source `GaussianData` to stay live
- keep existing in-memory/smoke paths working for small assets and tests
- make staged world IO the canonical large-world source path
- add one proof or stress path that demonstrates chunk payloads can be streamed without holding the full dataset in RAM

Primary files:

- `modules/gaussian_splatting/core/gaussian_streaming.h`
- `modules/gaussian_splatting/core/gaussian_streaming.cpp`
- `modules/gaussian_splatting/core/streaming_asset_types.h`
- `modules/gaussian_splatting/io/gaussian_splat_world_io.cpp`
- `modules/gaussian_splatting/persistence/*`
- relevant benchmark/proof fixtures and staging helpers

Non-goals:

- rewriting the renderer
- changing chunk residency policy by itself
- changing benchmark thresholds before the storage path exists

Validation:

- a staged large-world path loads chunk payloads on demand without retaining the full source world payload in memory
- process memory no longer scales linearly with total world splat count on the canonical staged path

Done when:

- large-world staged scenes are truly chunk-payload streamed, not just VRAM-streamed from a full in-memory source asset

## 6. Streaming: Tighten Pressure Control, Recovery, And Backlog Metrics For Sustained Open-World Load

Priority: `P2`

Depends on:

- `Streaming: make readiness and progress track atlas-published GPU truth`

Unblocks:

- believable long-soak behavior
- better tuning on reference hardware
- clearer evidence surfaces for large-world claims

Problem:

Pressure and recovery logic is still brittle under sustained load. Prefetch is aggressively shut off under pressure, forced sync fallback only triggers for one narrow signature, and backlog telemetry underreports pending work when pack work is the bottleneck.

Scope:

- align backlog telemetry with actual pack + upload + resident state
- tighten recovery heuristics for sustained pressure and churn
- make long-soak evidence easier to interpret without hand-reading traces
- preserve the Phase 4A ownership model and Phase 4B batching/coalescing work

Primary files:

- `modules/gaussian_splatting/core/gaussian_streaming.cpp`
- `modules/gaussian_splatting/core/streaming_upload_pipeline.cpp`
- `modules/gaussian_splatting/core/performance_monitors.cpp`
- `tests/runtime/test_gpu_streaming_stress.gd`
- `tests/runtime/test_gpu_streaming_eviction_churn_probe.gd`
- `docs/operations/streaming-production-playbook.md`

Non-goals:

- using higher default budgets to hide structural stalls
- replacing the upload pipeline ownership model
- changing the claim boundary before the proof surfaces are real

Validation:

- backlog metrics match actual pending work
- long-soak runs can distinguish bandwidth limits, residency limits, and churn
- recovery logic no longer depends on one narrow stall signature

Done when:

- sustained-load telemetry and recovery are reliable enough to support weekly large-world evidence runs

## Opening Order

Open and execute the issues in this order:

1. `Streaming: make world submissions first-class instance-pipeline assets`
2. `Streaming: make readiness and progress track atlas-published GPU truth`
3. `Streaming: size cull and sort capacity from real working set, not atlas upper bound`
4. `Streaming: replace O(total chunks) visibility discovery with a spatially bounded index`
5. `Streaming: move large-world chunk payloads out of full in-memory GaussianData`
6. `Streaming: tighten pressure control, recovery, and backlog metrics for sustained open-world load`

The first two are proof-path blockers. The next two are renderer/scaling corrections. The last two decide whether the architecture can honestly support large open-world scenes at the product level.

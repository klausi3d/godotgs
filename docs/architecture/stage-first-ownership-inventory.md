# Stage-First Ownership Inventory

W0.2 artifact for issue #356, refreshed against `origin/master` at
`d89bdd5d42` on 2026-05-20.

This document records the current ownership boundary, single-writer rules, and hidden mutable state for the four highest-value decomposition targets:

- `GaussianStreamingSystem`
- `TileRenderer`
- `GPUSortingPipeline`
- `GaussianSplatRenderer`

The goal is to make the current implementation readable as an ownership map
before code is moved. This is not the target end-state; it is the contract the
refactor has to preserve before route-controller extraction. Items previously
listed as pending but already moved in the live tree are called out as done, so
future PRs do not re-implement stale work.

## Ownership Rules

- One subsystem owns each mutable state bucket.
- Worker threads may produce payloads, but they do not mutate renderer-owned state directly.
- Render-thread dispatch, async readback, and resource tracking must have a single authoritative owner.
- Hidden process-global caches are temporary debt unless they are explicitly assigned to an owner.
- Existing seams are treated as contracts: do not replace `IRenderThreadDispatcher`,
  `ISortResultSink`, `ISortBufferHostContext`, or member-owned tile caches with
  duplicate cleanup work.

## Current Ownership Matrix

| System | Current owner | Mutable state buckets | Single-writer rule | Extraction risk |
| --- | --- | --- | --- | --- |
| `GaussianStreamingSystem` | `core/gaussian_streaming.*` | `VisibilityState`, `EvictionState`, `SchedulerState`, `DiagnosticsState`, `GlobalAtlasState`, `ConfigOverrides`, `PackTelemetry`, `frame_data`, `uploads`, `budget`, `chunks`, `atlas_allocator`, quantization buffers, `memory_stream_proxy`, `last_upload_device` | Main streaming update path owns frame ordering, visibility, eviction, upload scheduling, and atlas sync. Pack workers only build payloads for queued jobs and do not own streaming state. | High. Configuration overrides, frame ordering, and atlas sync are all coupled in `_run_streaming_frame_pipeline()` and `_apply_config_overrides()`. |
| `TileRenderer` | `renderer/tile_renderer.*` plus its stage helpers | `render_settings`, `shader_resources`, `diagnostics`, `async_readback`, `tracked_device_manager`, tracked output RIDs, `adaptive_overlap_budget_runtime_state`, `subgroup_support_cache`, `TileResourceController` | The renderer owns tile frame execution. Readback callbacks may update the readback state machine, but they must not take ownership of resource lifetime or output tracking. | Medium. The old process-global adaptive-overlap and subgroup caches are now members, but execution, diagnostics, and resource lifetime are still tightly packed in the tile renderer. |
| `GPUSortingPipeline` | `interfaces/gpu_sorting_pipeline.*` | Sort buffer RIDs and capacities, depth compute RIDs, CPU byte buffers, `SortReadbackState`, `InstanceCountReadbackState`, `InstancePipelineInputs`, cached camera transforms, `last_compute_error`, sink/context pointers | The pipeline owns its sort buffers, depth resources, and readback generations. Renderer interaction must stay behind `ISortResultSink` and `ISortBufferHostContext` except for the still-open instance input seam. | High. `InstancePipelineInputs::owner_renderer` and orchestrator-bound renderer sink/context wiring still couple sorting to renderer lifetime. |
| `GaussianSplatRenderer` | `renderer/gaussian_splat_renderer.*` | `DeviceState`, `ResourceState`, `PerformanceState`, `PipelineState`, `FrameState`, `ViewState`, `SortingState`, `StreamingState`, `SceneState`, `SubsystemState`, `RenderFrameContextManager`, `IRenderThreadDispatcher`, orchestrator pointers | The renderer owns frame entry, orchestration, and Godot-facing API. Concrete subsystem mutation should happen through orchestrators or service objects, not through direct cross-subsystem writes. | High. The dispatch state is now a service, but `render_scene_instance()`, `FrameStateProvider`, and `RenderPipelineStages` still expose broad ownership surfaces. |

## Subsystem Details

### GaussianStreamingSystem

Current state is already split into named buckets in `core/gaussian_streaming.h`:

- `VisibilityState` at `gaussian_streaming.h:215`
- `EvictionState` at `gaussian_streaming.h:264`
- `PackTelemetry` at `gaussian_streaming.h:294`
- `SchedulerState` at `gaussian_streaming.h:584`
- `DiagnosticsState` at `gaussian_streaming.h:642`
- atlas and quantization state at `gaussian_streaming.h:706` through `gaussian_streaming.h:739`

Current write ownership:

- `VisibilityState` is updated from the main streaming frame pipeline.
- `EvictionState` is updated by eviction helpers only.
- `SchedulerState` is updated by the per-frame orchestration path.
- `GlobalAtlasState` and atlas RIDs are updated only at the end of the streaming frame pipeline.
- Pack workers only create or transform `PackJob` and `PendingChunkUpload` payloads, then hand results back to the system.

Implementation anchors:

- Frame ordering is explicit in `_run_streaming_frame_pipeline()` and ends with `_sync_global_atlas_state()`.
- Runtime config overrides are applied in `_apply_config_overrides()`, where they touch `visibility`, `uploads`, and `budget` together.

Refactor note:

- W1 and W2 should not split `VisibilityState`, `EvictionState`, or `SchedulerState` until the ownership matrix is stable and the frame-order tests are pinned.

### TileRenderer

Current state is member-owned:

- Adaptive overlap runtime state lives in `TileRenderer::adaptive_overlap_budget_runtime_state` (`tile_renderer.h:430`) plus its initialized flag.
- Subgroup support cache lives in `TileRenderer::subgroup_support_cache` (`tile_renderer.h:423`).
- Resource ownership and tracking are handled by `TileResourceController`, `_resolve_texture_owner()`, `track_output_resources()`, and `clear_output_resource_tracking()`.

Current write ownership:

- The tile renderer owns the frame execution path and the tile stage helpers.
- Adaptive-overlap state is instance-owned by `TileRenderer`.
- Subgroup-support cache is instance-owned by `TileRenderer`; it is a device capability cache, not a process-global mutable map.
- Output RID tracking belongs to the renderer instance and the `RenderDeviceManager`, not to the caller.

Implementation anchors:

- `track_output_resources()` and `clear_output_resource_tracking()` own the current output-RID lifecycle.
- `_resolve_texture_owner()` is the contract gate for cross-device resource ownership.

Refactor note:

- Do not implement the old "move static tile caches" task; that is already done.
- Remaining tile work should be leaf cleanup: narrow resource-controller calls and split execution/diagnostics only after route and frame-state contracts are pinned.

### GPUSortingPipeline

Current state in `interfaces/gpu_sorting_pipeline.h` is concentrated but still too coupled:

- Sort buffers and sort capacity live at `gpu_sorting_pipeline.h:132` through `gpu_sorting_pipeline.h:145`
- Depth compute resources live at `gpu_sorting_pipeline.h:150` through `gpu_sorting_pipeline.h:176`
- CPU-side sort and depth buffers live at `gpu_sorting_pipeline.h:178` through `gpu_sorting_pipeline.h:183`
- `SortReadbackState` and `InstanceCountReadbackState` live at `gpu_sorting_pipeline.h:197` through `gpu_sorting_pipeline.h:212`
- `ISortResultSink *sort_result_sink` and `ISortBufferHostContext *sort_buffer_host_context` live at `gpu_sorting_pipeline.h:247` through `gpu_sorting_pipeline.h:248`
- `InstancePipelineInputs::owner_renderer` still lives at `gpu_sorting_pipeline.h:40`

Current write ownership:

- The pipeline owns its RIDs, capacities, and readback generation counters.
- `sort_readback_state` controls publication of sorted results.
- `instance_count_readback_state` controls publication of instance count readbacks.
- Sort result publication is routed through `ISortResultSink`; sort buffer sizing and host state are routed through `ISortBufferHostContext`.
- Instance sorting still receives renderer lifetime through `InstancePipelineInputs::owner_renderer`.

Implementation anchors:

- `_apply_sorted_results()` now writes through `ISortResultSink`, not `pending_renderer`.
- `ensure_sort_buffers()` now uses `ISortBufferHostContext` rather than taking a renderer pointer.
- `RenderSortingOrchestrator::_sync_instance_sort_inputs()` still copies `owner_renderer` into `InstancePipelineInputs`.
- `RenderSortingOrchestrator::_bind_sort_pipeline_host_context()` still binds both sink and host context to the renderer object.
- `clear_instance_pipeline_inputs()` and the readback reset path are the current state reset boundary for instance-pipeline work.

Refactor note:

- The `ISortResultSink` and `ISortBufferHostContext` seam is already live; do not duplicate it.
- The next sorting ownership step is replacing `InstancePipelineInputs::owner_renderer` with an instance-state view/snapshot that preserves async readback and generation semantics.
- W2 should extract sort buffer, depth compute, execution, and validation stages only after that instance seam is pinned by tests.

### GaussianSplatRenderer

Current state is spread across explicit sub-buckets and orchestrators:

- Frame and view state are grouped in `RenderFrameContextManager`
- `DeviceState` is the owner of the current render-device and scene-render handles
- `ResourceState` owns buffer lifecycle and per-frame GPU resource state
- `SubsystemState` owns orchestrator and interface handles
- `RenderThreadDispatcher` owns mutex/semaphore/request state behind `IRenderThreadDispatcher`
- `FrameStateProvider` exposes broad read and mutable access across scene, streaming, debug, sorting, config, resource, frame, performance, subsystem, and pipeline feature state

Implementation anchors:

- `interfaces/render_thread_dispatcher.h:10` through `:25` define the dispatch interface and `:44` through `:51` hold the owned mutex/semaphore/request state.
- `_dispatch_call_on_render_thread_blocking()` and `_notify_render_thread_dispatch_completed()` delegate to the dispatcher.
- `render_scene_instance()` still owns per-frame cleanup, camera extraction, route policy diagnostics, resident/streaming selection, and typed skip publication.
- `RenderPipelineStages::prepare_frame_context()` still builds `FrameDeps` by reaching broadly into renderer state buckets.
- `RenderPipelineStages::execute_frame_entry()` still owns the cull -> sort cascade and zero-safe snapshot updates.
- `RenderPipelineStages::render_sorted_splats_with_context()` still resets raster/GPU/output-cache metrics before raster/composite.
- `gaussian_splat_renderer.cpp:156` through `gaussian_splat_renderer.cpp:163` hold the frame-log settings cache.

Current write ownership:

- The renderer owns Godot-facing API entry points, frame setup, orchestration, and cross-subsystem lifecycle.
- Orchestrators own the finer-grained subsystem work that has already been peeled out.
- Render-thread dispatch state is dispatcher-owned; remaining work is call-site policy and teardown/data-submit ownership.
- `render_scene_instance()` remains the route/frame-entry knot and should be the first extraction target after this W0 characterization PR.
- `FrameStateProvider` should become a temporary adapter over smaller snapshots/sinks, not a renamed god object.

Refactor note:

- W1 should remove synchronization from the renderer facade.
- W2 should continue slimming the facade, but only after W1 has made ownership explicit.

## Hidden Mutable State Inventory

| Location | Symbol / state | Why it matters | Current owner status |
| --- | --- | --- | --- |
| `renderer/gaussian_splat_renderer.cpp:156` | `g_frame_log_settings` | Process-global cache for frame logging/debug behavior. It is updated from project settings and shared by all renderer instances. | Temporary global cache, should become explicit renderer or process service state. |
| `interfaces/render_thread_dispatcher.h:44` through `:51` | dispatcher mutex, semaphore, request/completion counters, timeout, latest data result | Synchronization state is now service-owned instead of embedded in the renderer facade. | Done as an ownership move. Remaining coupling is renderer call-site policy, not field ownership. |
| `interfaces/gpu_sorting_pipeline.h:40` | `InstancePipelineInputs::owner_renderer` | Cross-object lifetime handoff remains for instance sorting, even after sink/host extraction. | Open coupling; replace with an instance-state snapshot/view after characterization tests. |
| `interfaces/gpu_sorting_pipeline.h:247` through `:248` | `sort_result_sink`, `sort_buffer_host_context` | Explicit sort result and buffer-host seams. | Done seam, but currently bound to the renderer by `RenderSortingOrchestrator`; future work should narrow the concrete host object. |
| `renderer/gaussian_splat_renderer.cpp:603` through `:770` | `FrameStateProvider` fallback statics and broad mutable accessors | Provider methods can expose fallback mutable objects and broad write surfaces. | Open coupling; split into read-only snapshots plus small mutation sinks per stage. |
| `nodes/gaussian_splat_node_helpers.cpp:33` through `:78` | shared renderer settings owner map | Node-side renderer settings ownership is process-global arbitration and protects shared renderer peers. | Intentional external contract for now; moving it belongs in a later node/director boundary issue. |
| `core/gaussian_streaming.cpp:2400` | `debug_frame` static counter | Telemetry pacing is process-global, not instance-owned. | Benign but still hidden mutable state; keep the scope explicit. |

## What This Means For Decomposition

- `W0` must stay stable: the ownership matrix and the characterization tests are the hard gate.
- `W1` must not redo completed seam work. The live done list is: render-thread dispatcher state, sort result sink, sort buffer host context, member-owned tile subgroup cache, and member-owned tile adaptive-overlap runtime state.
- `W1` should cut the still-open seams first: route/frame-entry controller contract, smaller frame-state snapshots/sinks, sorting instance-state snapshot, and resident publisher product boundaries.
- `W2` should extract execution stages only after ownership is explicit and characterization tests protect route/stage stats, fallback semantics, instance generations, shared renderer settings, dispatch recovery, and resource-owner rejection.
- `W3` should be cleanup only: remove the shims, slim the facades, and enforce the dependency rules.

## Related Docs

- [Architecture overview](overview.md)
- [Render pipeline details](render-pipeline.md)
- [Module architecture map](../../modules/gaussian_splatting/ARCHITECTURE.md)
- [Memory and residency invariants](../../modules/gaussian_splatting/MEMORY_SUBSYSTEM.md)

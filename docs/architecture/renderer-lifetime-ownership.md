# Renderer Lifetime Ownership

PR 2 of work package #352 (Renderer Lifetime Proof). Anchored to `master` at
`b7e7f05a77` on 2026-05-22.

## Purpose

This document enumerates every GPU-resource owner inside the `gaussian_splatting`
module with `file:line` precision, recording who creates each resource, who
destroys it, whether the destruction path is idempotent, and what
mutexes/threading rules guard the owner. It exists because the F6 reload leak
documented at `modules/gaussian_splatting/core/gaussian_splat_scene_director.cpp:351`
and the 602-event failed-init crash cascade can only be reasoned about against
a single authoritative ownership map, and no such map existed before this PR.

The doc complements:

- [`modules/gaussian_splatting/ARCHITECTURE.md`](../../modules/gaussian_splatting/ARCHITECTURE.md)
  — subsystem composition and data flow.
- [`modules/gaussian_splatting/MEMORY_SUBSYSTEM.md`](../../modules/gaussian_splatting/MEMORY_SUBSYSTEM.md)
  — VRAM-budget policy and persistent-buffer sizing.
- [`docs/architecture/stage-first-ownership-inventory.md`](stage-first-ownership-inventory.md)
  — stage-level write-ownership for the decomposition work package (#356).

What those three docs leave undocumented — and what this doc adds — is the
**destruction contract and idempotency** of each owner. That is the missing
piece the rest of work package #352 needs.

## How to read this doc

Every owner subsection has six fields:

1. **Owner identity** — the class name and `file:line` of the storage
   declaration in the header.
2. **What it owns** — the concrete GPU resources (RIDs, buffers, textures,
   pipelines, framebuffers, shaders, scratch pools, etc.) by storage-member
   name.
3. **Created where** — the function and `file:line` that allocates the
   resource, with thread context (render thread vs. main thread) and
   eager-vs-lazy noted.
4. **Destroyed where** — the destructor or explicit shutdown function and
   `file:line`, with an explicit note on whether the cleanup is idempotent.
5. **Threading / locking** — the mutexes the owner acquires, including the
   shared `GaussianSplatManager` L1-L4 lock hierarchy at
   `gaussian_splat_manager.h:54-71` when applicable.
6. **Known leak vectors / TODOs** — file:line citations for known leak vectors
   or open contract gaps in the current code.

Line numbers are anchored to `b7e7f05a77`. PRs 3/4/5/6 that consume this doc
should re-confirm any line citation they quote.

## Top-level ownership graph

```
GaussianSplatManager (singleton)                    [core/gaussian_splat_manager.h:73]
├── primary_local_device                            [render thread]
├── shared_submission_device                        [render thread]
├── gaussian_buffers (HashMap<ObjectID, BufferEntry>)
├── buffer_lookup / gaussian_buffer_owner_devices
├── dynamic_asset_cache / dynamic_asset_owner_devices
├── active_nodes (HashSet<ObjectID>)
└── submission_mutex (static SafeBinaryMutex)

GaussianSplatSceneDirector (singleton)              [core/gaussian_splat_scene_director.h:26]
└── worlds: HashMap<RID, SharedWorld>               [scene_director.h:356]
    └── SharedWorld                                  [scene_director.h:319]
        ├── Ref<GaussianSplatRenderer> renderer
        ├── instances + sphere_effectors + world_submission + asset_records
        └── (destructor relies on worlds.clear() in director dtor:353)

GaussianSplatRenderer                               [renderer/gaussian_splat_renderer.h:144]
├── SubsystemState                                  [renderer/render_types/render_facade_state_types.h:135]
│   ├── Ref<RenderDeviceManager>          ── owns resource_owner_map etc.
│   ├── Ref<DebugOverlaySystem>
│   ├── Ref<InteractiveStateManager>
│   ├── Ref<TileRasterizer>
│   ├── Ref<GPUCuller>                    ── owns cull shader/pipeline/buffers
│   ├── Ref<OutputCompositor>             ── owns framebuffer cache + scratch pool
│   ├── Ref<GPUSortingPipeline>
│   ├── Ref<OverflowAutoTuner>
│   ├── Ref<PainterlyRenderer>
│   └── Ref<PainterlyMaterialManager>
├── ResourceState                                   [render_facade_state_types.h:46]
│   ├── Ref<GPUBufferManager> buffer_manager
│   ├── DeferredDeletionQueue deletion_queue
│   ├── instance_buffer / instance_grading_buffer / instance_visible_chunk_buffer / …
│   └── resident_atlas_*_buffer (4 atlas-shape buffers)
├── StreamingState                                  [render_types/render_state_types.h:77]
│   ├── Ref<GaussianMemoryStream> memory_stream
│   ├── Ref<GaussianStreamingSystem> current_streaming_system
│   └── registered_gaussian_buffer (handed back to manager on teardown)
├── SortingState                                    [render_state_types.h:114]
│   └── Ref<IGPUSorter> gpu_sorter
├── TileRendererState                               [render_facade_state_types.h:129]
│   ├── Ref<TileRenderer> renderer
│   └── GPUPerformanceMonitor
├── PipelineState                                   [gaussian_splat_renderer.h:200]
│   └── gaussian_shader + gaussian_shader_version
├── ShadowBlitState                                 [render_facade_state_types.h:148]
└── Ref<OutputCompositor> shadow_output_compositor  [renderer/gaussian_splat_renderer.h:732]

GaussianStreamingSystem                             [core/gaussian_streaming.h:29]
├── persistent_buffer                                [gaussian_streaming.h:90]
├── chunks[*].gpu_buffer                             [gaussian_streaming.h:86]
├── quantization buffers                             (via streaming_quantization.h)
└── StreamingGlobalAtlasRegistry global_atlas_registry [gaussian_streaming.h:83]
    ├── asset_meta_buffer                            [streaming_global_atlas_registry.h:88]
    ├── chunk_meta_buffer                            [streaming_global_atlas_registry.h:89]
    └── asset_chunk_index_buffer                     [streaming_global_atlas_registry.h:90]

TileRenderer                                        [renderer/tile_renderer.h]
├── TileRenderTargets (output/depth/normal/resolved)
├── TileShaderResources (binning/prefix/raster/resolve shaders + pipelines)
├── TileGlobalSortResources (sort scratch + per-tile counts)
├── TileUniformBuffers / TileProjectionBuffers
├── TileSHCacheBuffers / TileSubpixelHistoryBuffers / TileSubpixelVisibilityBuffers
└── TileResourceController (tracks output color/depth)

GaussianSplatNode3D (per-instance, scene-side)      [nodes/gaussian_splat_node_3d.h]
├── Ref<GaussianSplatRenderer> renderer             [node_3d.h:230]
├── render_instance (RS instance RID)               [node_3d.h:207]
├── gaussian_base (RS base RID)                     [node_3d.h:208]
└── cached_viewport_render_target / texture         [node_3d.h:220-222]
```

## Owners

### GaussianSplatManager

1. **Owner identity:** `class GaussianSplatManager` at
   `modules/gaussian_splatting/core/gaussian_splat_manager.h:73`. Process-wide
   singleton (`singleton` member at `:88`).
2. **What it owns:**
   - `primary_local_device` (`std::atomic<RenderingDevice *>` at `:118`).
   - `shared_submission_device` (`std::atomic<RenderingDevice *>` at `:120`).
   - GPU-buffer registry `gaussian_buffers` (`HashMap<ObjectID, BufferEntry>`
     at `:104`) and the parallel `buffer_lookup` (`:105`) and
     `gaussian_buffer_owner_devices` (`:106`) maps.
   - Dynamic asset cache `dynamic_asset_cache` (`:107`) and
     `dynamic_asset_owner_devices` (`:108`).
   - `active_nodes` (`HashSet<ObjectID>` at `:111`) — registered nodes for
     per-frame processing.
   - The static `submission_mutex` (`SafeBinaryMutex<SUBMISSION_MUTEX_TAG>`
     at `:132`).
   - The deferred device-destroy dispatcher
     `local_device_destroy_dispatcher` (`:127`) plus pending pointers at
     `:128`-`:129`.
3. **Created where:**
   - Singleton instantiated by `register_types.cpp:117` during
     `MODULE_INITIALIZATION_LEVEL_SCENE`.
   - Local `RenderingDevice` instances created lazily on the render thread via
     `_create_local_device_on_render_thread()` (called from
     `_request_primary_local_device()` / `_request_shared_local_device()`,
     wrapped by `GS_STARTUP_SCOPE("device_request_primary")` at
     `gaussian_splat_manager.cpp:255` and `"device_request_shared"` at
     `:259`).
   - Constructor itself wrapped by `GS_STARTUP_SCOPE("manager_construct")` at
     `gaussian_splat_manager.cpp:219`.
4. **Destroyed where:**
   - Destructor at `gaussian_splat_manager.cpp:666` performs sequential
     teardown: `_disconnect_frame_callbacks()` →
     `active_nodes.clear()` under `active_nodes_mutex` →
     `_release_registered_resources()` (line `:676`) →
     `_destroy_local_devices()` (line `:677`) → `singleton = nullptr`.
   - `finalize_module()` at `gaussian_splat_manager.cpp:1194` is called from
     `register_types.cpp:242` and is a soft shutdown
     (`_release_registered_resources` + `active_nodes.clear` +
     `_disconnect_frame_callbacks`) that the destructor's full path then
     finishes.
   - **Idempotent:** Yes for `_release_registered_resources()` (uses
     `registered_resources_released` flag at `:110`) and
     `_disconnect_frame_callbacks()` (gated by
     `frame_callbacks_connected` at `:150`). The local-device destroy path
     is single-shot under its own mutex.
5. **Threading / locking:** This is the canonical lock-hierarchy owner.
   - Level 1 `submission_mutex` (static, `:132`).
   - Level 2 `resource_maps_mutex` (`Mutex` at `:109`) — protects
     `gaussian_buffers`, `buffer_lookup`, `dynamic_asset_cache`, and the
     two `*_owner_devices` maps.
   - Level 3 `active_nodes_mutex` (`Mutex` at `:112`).
   - Level 4 `local_device_destroy_request_mutex` (`:125`) and
     `local_device_destroy_pending_mutex` (`:126`).
   - The full ordering and rules are spelled out in the header comment at
     `gaussian_splat_manager.h:54-71`. DEV_ENABLED builds runtime-validate
     via `_gs_lock_level_guard`.
6. **Known leak vectors / TODOs:**
   - Local-device creation requests are deferred to the render thread via
     `_dispatch_local_device_destroy_on_render_thread` (see
     `gaussian_splat_manager.h:145`); if the render thread is gone before
     the dispatch lands, the devices fall through to
     `_destroy_local_devices_immediate` (`:146`). This is the contract PR 1
     hardens against init-failure cascades.

### GaussianSplatSceneDirector

1. **Owner identity:** `class GaussianSplatSceneDirector` at
   `modules/gaussian_splatting/core/gaussian_splat_scene_director.h:26`.
   Singleton at `:353`.
2. **What it owns:**
   - `worlds`: `HashMap<RID, SharedWorld>` at `scene_director.h:356`. Each
     `SharedWorld` (`:319`) holds:
     - `Ref<GaussianSplatRenderer> renderer` at `:321`. **This is the strong
       reference whose release is the only path that drops the renderer and
       its entire GPU-resource graph.**
     - Instance records, sphere effectors, world-submission record, and
       asset records (`asset_records` `HashMap` at `:350`).
   - `scene_effector_multi_match_warned_nodes` (`HashSet<ObjectID>` at `:357`)
     — diagnostic state, no GPU resources.
3. **Created where:**
   - Singleton instantiated at `register_types.cpp:147`.
   - `SharedWorld` entries created lazily by
     `_get_or_create_world_for_scenario()` at
     `gaussian_splat_scene_director.cpp:362`. The renderer for the world is
     constructed eagerly there at line `:390` (`Ref<GaussianSplatRenderer>(memnew(GaussianSplatRenderer(device)))`).
4. **Destroyed where:**
   - Destructor at `gaussian_splat_scene_director.cpp:348` clears `worlds`
     (line `:353`). The intent is captured in the comment that begins at
     line `349` and continues through line `352`:
     ```
     // Release all SharedWorld entries so their Ref<GaussianSplatRenderer>
     // instances are unreferenced, allowing GPU resources (compute/shader
     // RIDs, buffers) to be freed.  Without this, each F6 runtime cycle
     // leaks an entire renderer's worth of GPU allocations.
     ```
     The literal text at line 351 is: `// RIDs, buffers) to be freed.  Without this, each F6 runtime cycle`.
   - Per-scenario pruning is also wired via `_prune_world_if_unused()`
     declared at `scene_director.h:396` (called from instance/effector
     unregister paths once the world has nothing live left to track).
   - **Idempotent:** `worlds.clear()` is idempotent; subsequent destructor
     calls (impossible — singleton) would no-op.
5. **Threading / locking:** `mutable Mutex world_mutex` at
   `scene_director.h:355`. **Every** mutating and most reading methods take
   this lock; nested lock acquisition with `GaussianSplatManager` locks is
   never performed (the director does not enter the manager's L1-L4
   hierarchy while holding `world_mutex`).
6. **Known leak vectors / TODOs:**
   - **F6 reload leak:** Documented in the destructor comment cited above.
     If the director is leaked (not destroyed at module finalization), every
     `SharedWorld` keeps its renderer alive and the renderer's entire GPU
     graph leaks. Module shutdown at `register_types.cpp:250`
     (`memdelete(gaussian_splat_scene_director_singleton)`) is the only
     normal release path. **This is the canonical leak vector PR 4
     (explicit SharedWorld teardown contract) formalizes.**
   - The renderer creation path at `scene_director.cpp:390` runs even when
     no `RenderingDevice` is available (early-returns at `:387` with a warn).
     The retry on next `_get_or_create_world_for_scenario()` call relies on
     `entry->renderer.is_valid()` being false — i.e. the
     `worlds.insert(p_scenario, world)` at `:371` leaves an entry that other
     code paths must tolerate without a renderer.

### GaussianSplatRenderer

1. **Owner identity:** `class GaussianSplatRenderer` (RefCounted) at
   `modules/gaussian_splatting/renderer/gaussian_splat_renderer.h:144`. Storage
   members live at `:595`-`:624` (state buckets and orchestrators) plus
   `:732` (`shadow_output_compositor`).
2. **What it owns:** Composes the following sub-owners, each via a `Ref<>` in
   `SubsystemState` (`render_facade_state_types.h:135`), `ResourceState`
   (`:46`), `StreamingState` (`render_state_types.h:77`), `SortingState`
   (`:114`), or `TileRendererState` (`:129`):
   - `SubsystemState::device_manager` (Ref<RenderDeviceManager>) at
     `render_facade_state_types.h:136`.
   - `SubsystemState::debug_overlay_system`, `interactive_state_manager`,
     `rasterizer`, `gpu_culler`, `output_compositor`, `sorting_pipeline`,
     `overflow_auto_tuner`, `painterly_renderer`,
     `painterly_material_manager` at `:137`-`:145`.
   - `ResourceState::buffer_manager` (Ref<GPUBufferManager>) at `:49`.
   - `ResourceState::deletion_queue` (`GPUBufferManager::DeferredDeletionQueue`)
     at `:51`.
   - `ResourceState` instance-pipeline buffers
     (`instance_buffer`, `instance_grading_buffer`,
     `instance_visible_chunk_buffer`, `instance_splat_ref_buffer`,
     `instance_counter_buffer`, `instance_chunk_dispatch_buffer`,
     `instance_indirect_count_buffer`, `instance_count_buffer`) at `:52`-`:79`.
   - Resident atlas mirror buffers
     (`resident_atlas_gaussian_buffer`, `resident_asset_meta_buffer`,
     `resident_asset_chunk_index_buffer`, `resident_chunk_meta_buffer`,
     `resident_quantization_buffer`) at `:80`-`:89`.
   - `StreamingState::memory_stream` (Ref<GaussianMemoryStream>) at `:78` and
     `current_streaming_system` (Ref<GaussianStreamingSystem>) at `:79`.
   - `StreamingState::registered_gaussian_buffer` at `render_state_types.h:84`
     — the RID handed back to the manager on teardown.
   - `SortingState::gpu_sorter` (Ref<IGPUSorter>) at `:115`.
   - `TileRendererState::renderer` (Ref<TileRenderer>) at `:130`.
   - `PipelineState::gaussian_shader` / `gaussian_shader_version` at
     `gaussian_splat_renderer.h:201`-`:202`.
   - `TestDataState` vertex/position/scale/rotation/sh buffers at
     `render_facade_state_types.h:119`-`:123`.
   - `shadow_output_compositor` (Ref<OutputCompositor>) at
     `gaussian_splat_renderer.h:732`.
   - `ShadowBlitState` shader/pipeline cache/sampler at
     `render_facade_state_types.h:148`-`:158`.
3. **Created where:**
   - Constructor at `gaussian_splat_renderer.cpp:870`, wrapped by
     `GS_STARTUP_SCOPE("renderer_construct")` at line `:871`. Called from
     `GaussianSplatSceneDirector::_get_or_create_world_for_scenario()` at
     `gaussian_splat_scene_director.cpp:390`. Sub-owners are instantiated
     lazily during `_initialize_on_render_thread()` (declared at
     `gaussian_splat_renderer.h:661`).
4. **Destroyed where:**
   - Destructor at `gaussian_splat_renderer.cpp:1216` performs the
     project-settings disconnect, diagnostics unregister, and performance
     monitor unregister, then attempts
     `_dispatch_call_on_render_thread_blocking(_teardown_on_render_thread)`.
     If the dispatch is rejected, it falls back to a synchronous
     `_teardown_resources()` (line `:1239`).
   - `_teardown_resources()` at `gaussian_splat_renderer.cpp:1242` is the
     full release path. It is **explicitly idempotent**: a
     `std::atomic<bool> teardown_resources_started` (at
     `gaussian_splat_renderer.h:624`) is `compare_exchange_strong`-flipped
     at line `:1244`; second entry returns immediately.
   - Order of release inside `_teardown_resources()`:
     - `_release_shared_dynamic_asset()` (`:1248`) →
       `get_resource_state().deletion_queue.flush_all()` (`:1249`).
     - Both `OutputCompositor`s shutdown + `clear_cached_framebuffers()` +
       `clear_viewport_blit_resources()` (`:1253`-`:1264`).
     - `streaming_state.registered_gaussian_buffer` handed back to the
       manager via `unregister_gaussian_buffer()` (`:1271`-`:1279`).
     - `TileRenderer::cleanup()` (`:1284`).
     - `StreamingState::current_streaming_system.unref()` (`:1293`) and
       `memory_stream->shutdown()` + `unref()` (`:1296`-`:1297`).
     - `gpu_culler->shutdown()` (`:1304`), `interactive_state_manager`
       shutdown (`:1308`), `debug_overlay_system` shutdown (`:1312`),
       `sorting_pipeline->release_sort_buffers()` + `shutdown()`
       (`:1318`-`:1319`), `painterly_renderer->free_painterly_resources()`
       (`:1324`).
     - `buffer_manager.unref()` (`:1329`),
       `gpu_sorter->shutdown()` (`:1334`).
     - `_release_resident_contract_buffers()` (`:1338`, defined at `:1387`)
       releases the 4 resident_atlas_*_buffer RIDs.
     - Direct `_free_owned_resource()` calls for the instance-pipeline
       buffers and TestData buffers (`:1349`-`:1365`).
     - `pipeline_state.gaussian_shader_source` `memdelete` (`:1371`).
     - `device_manager->shutdown()` last (`:1377`).
   - **Idempotent:** Yes — `teardown_resources_started` guard at `:1244`
     guarantees single-shot.
5. **Threading / locking:** Teardown is dispatched onto the render thread
   when possible (see `IRenderThreadDispatcher` contract at
   `interfaces/render_thread_dispatcher.h:10`-`:25`). The renderer itself
   does not own a process-wide mutex; orchestrators and sub-owners use their
   own locks (e.g. `GaussianSplatManager::ScopedSubmissionLock` via
   `acquire_submission_lock()`).
6. **Known leak vectors / TODOs:**
   - If the renderer is leaked through its `Ref<>` (e.g. director leak,
     see F6 leak vector), `_teardown_resources()` never runs and **every
     sub-owner above leaks transitively**. This is precisely why PR 4
     formalizes the director's SharedWorld teardown.
   - `RenderResourceOrchestrator` (`renderer/render_resource_orchestrator.*`,
     declared at `gaussian_splat_renderer.h:620`) initializes
     `GPUBufferManager` under `GS_STARTUP_SCOPE("gpu_buffer_manager_init")`
     at `render_resource_orchestrator.cpp:165`. Failed init must roll back to
     match `_teardown_resources()` expectations — this is PR 3's
     instrumentation surface.

### RenderDeviceManager

1. **Owner identity:** `class RenderDeviceManager` (RefCounted) at
   `modules/gaussian_splatting/interfaces/render_device_manager.h:51`. Field
   storage at `:124`-`:150`.
2. **What it owns:**
   - `main_rd`, `local_rd`, `submission_rd` device pointers (`:124`-`:126`).
   - `resource_owner_map` (`HashMap<uint64_t, RenderingDevice *>` at `:142`)
     and the parallel `resource_owner_instance_id_map` (`:143`),
     `resource_ownership_map` (`:144`), `resource_label_map` (`:145`),
     `resource_type_map` (`:146`).
   - `texture_owner_map` (`HashMap<uint64_t, RenderingDevice *>` at `:147`)
     — textures are tracked separately.
   - Diagnostic counters
     `last_shutdown_tracked_resource_count` (`:148`),
     `last_shutdown_owned_resource_count` (`:149`),
     `last_shutdown_borrowed_resource_count` (`:150`); accessors at
     `:115`-`:117`.
   - Diagnostics ring buffers `texture_trace` (`:153`),
     `cross_device_ops` (`:154`), capped at `MAX_TRACE_ENTRIES = 1000` (`:155`).
3. **Created where:** Default-constructed at
   `render_device_manager.cpp:84`. `initialize()` at `:91` binds the primary
   `RenderingDevice`. Instantiated by `RenderResourceOrchestrator` during
   renderer init (entry into `SubsystemState::device_manager`).
4. **Destroyed where:**
   - Destructor at `render_device_manager.cpp:87` calls `shutdown()`.
   - `shutdown()` at `:119`: snapshots `last_shutdown_*` counts
     (`:120`-`:122`), `ERR_PRINT`s if owned resources remain (`:123`-`:126`),
     clears all six tracking maps + texture/cross-device traces
     (`:128`-`:135`), drops device pointers (`:142`-`:144`), clears
     `owns_local_rd` (`:145`).
   - **Idempotent:** Yes — `shutdown()` re-runs are no-ops because all maps
     are already empty and pointers are already null.
5. **Threading / locking:** No internal mutex; the manager is expected to be
   accessed from a single owning thread context (the renderer's
   render-thread teardown path).
6. **Known leak vectors / TODOs:**
   - The `ERR_PRINT` at `:123`-`:126` is the only diagnostic that fires when
     a renderer drops the device manager with un-freed RIDs. PR 3's
     lifetime-proof fixture should assert
     `get_last_shutdown_owned_resource_count() == 0` after a clean teardown.

### OutputCompositor

1. **Owner identity:** `class OutputCompositor` (RefCounted) at
   `modules/gaussian_splatting/interfaces/output_compositor.h:22`. Field
   storage at `:222`-`:243`.
2. **What it owns:**
   - `output_cache.cached_framebuffers` — `HashMap<uint64_t, CachedFramebuffer>`
     at `output_compositor.h:115`. **Unbounded.** One entry per
     `(device, attachment-key)` combination; editor viewport reformats /
     device switches accumulate entries indefinitely.
   - `output_cache.framebuffer_validation_cache` — `HashMap` at `:116`,
     same unboundedness profile.
   - `viewport_blit_variants` (`HashMap<uint64_t, ViewportBlitVariant>` at
     `:235`) — shader + pipeline per `(device, format)`.
   - `viewport_blit_samplers` (`:236`).
   - `viewport_blit_scratch` (`HashMap<uint64_t, ViewportBlitScratch>` at
     `:237`) — bounded LRU; per-`(device, format)` entry cap is
     `VIEWPORT_BLIT_SCRATCH_MAX_PER_KIND = 4` at `output_compositor.h:220`.
   - `viewport_blit_shader_source` (`ViewportBlitShaderRD *` at `:238`).
   - `srgb_format_cache` (`HashMap<uint64_t, bool>` at `:232`).
   - `final_render_texture` RID (`:243`).
3. **Created where:** Constructed via `Ref<OutputCompositor>` inside the
   renderer's `SubsystemState` (`output_compositor` field at
   `render_facade_state_types.h:141`) and as `shadow_output_compositor` at
   `gaussian_splat_renderer.h:732`. `initialize(RenderingDevice *)` is the
   first-use entry point declared at `output_compositor.h:31`. Cached
   framebuffers are created lazily by `get_cached_framebuffer()` (`:40`).
   Scratch entries by `_ensure_viewport_blit_scratch()` (`:259`).
4. **Destroyed where:**
   - Destructor declared at `output_compositor.h:28`.
   - `shutdown()` declared at `:32`; renderer calls it from
     `_teardown_resources()` at `gaussian_splat_renderer.cpp:1256` (primary)
     and `:1262` (shadow), preceded by `clear_cached_framebuffers()` (`:41`)
     and `clear_viewport_blit_resources()` (`:42`).
   - **Idempotent:** Tracked via `bool initialized` at `output_compositor.h:224`
     and the `rd` device pointer at `:225`; `is_initialized()` (`:33`)
     gates re-entry.
5. **Threading / locking:** Single-owner; no internal mutex. Renderer
   serializes calls on the render thread.
6. **Known leak vectors / TODOs:**
   - **`cached_framebuffers` is unbounded** — see `output_compositor.h:115`
     and the comment block at `:216`-`:219` explaining why the scratch
     pool needed an LRU. The same reasoning applies to the framebuffer
     cache but the bound is not yet in place. **This is the canonical
     motivation for PR 5 (OutputCompositor LRU bound).**
   - `framebuffer_validation_cache` shares the same unboundedness profile
     and should be sized in the same change.

### GaussianStreamingSystem

1. **Owner identity:** `class GaussianStreamingSystem` (RefCounted) at
   `modules/gaussian_splatting/core/gaussian_streaming.h:29`. Field storage
   at `:67`-`:120`.
2. **What it owns:**
   - `persistent_buffer` (RID) at `gaussian_streaming.h:90`.
   - `persistent_buffer_size` and right-sizing counters
     (`streaming_initial_capacity`, `streaming_current_capacity`,
     `streaming_max_capacity`, `streaming_grow_count`) at `:91`-`:97`.
   - `chunks[*].gpu_buffer` (per-chunk RIDs in `LocalVector<StreamingChunk>`
     at `:86`).
   - Quantization buffer state (in `StreamingQuantization` aggregate;
     released by `_release_quantization_buffer()`).
   - `frame_data[RING_BUFFER_FRAMES]` ring (`:67`) with
     `RING_BUFFER_FRAMES = 3` at `:39`.
   - `atlas_allocator` (`GaussianAtlasAllocator` at `:82`) — CPU-side slot
     bookkeeping.
   - `global_atlas_registry` (`StreamingGlobalAtlasRegistry` at `:83`) — sub-owner
     of asset/chunk meta buffers (see next section).
   - `upload_pipeline` (`StreamingUploadPipeline` at `:75`) — pack threads
     and pending upload payloads.
3. **Created where:** Constructor at `gaussian_streaming.cpp:893`. The
   `persistent_buffer` is allocated by `initialize()` (anchored by
   `GS_STARTUP_SCOPE("streaming_persistent_buffer_alloc")` at
   `gaussian_streaming.cpp:1266`); growth in
   `_grow_persistent_buffer()` at `:967`. Atlas registry CPU state built at
   `GS_STARTUP_SCOPE("streaming_atlas_build_cpu")` at `:1314`; GPU sync at
   `"streaming_atlas_sync_gpu"` at `:1318`.
4. **Destroyed where:**
   - Destructor at `gaussian_streaming.cpp:921`:
     - `_layout_hint_reset_state()` + `_stop_pack_threads()` +
       `_clear_pending_uploads()` (`:922`-`:924`).
     - Resolves the upload device by falling back through
       `primary_device_override` → `last_upload_device` →
       `manager->get_primary_rendering_device()` (`:928`-`:930`).
     - `_release_persistent_buffer(rd, "destructor")` (`:931`) — defined at
       `:944`; guards against null-RID and null-device cases.
     - `global_atlas_registry.cleanup(rd)` (`:932`).
     - Loop frees each `chunk.gpu_buffer` (`:934`-`:938`).
     - `_release_quantization_buffer(rd, "destructor", false)` (`:941`).
   - **Idempotent:** `_release_persistent_buffer()` re-resets fields to
     zero on each entry, so a second destruction would no-op. Per-chunk
     frees guard on `chunk.gpu_buffer.is_valid()` (`:935`).
5. **Threading / locking:** Pack threads spun up by `upload_pipeline` are
   stopped explicitly at destructor entry (`:923`); single-thread main
   path otherwise.
6. **Known leak vectors / TODOs:**
   - The destructor depends on resolving a `RenderingDevice` at line `:928`.
     If both `primary_device_override` and `last_upload_device` are null and
     the manager singleton is already gone (rare during partial shutdown),
     `_release_persistent_buffer()` `WARN_PRINT`s and drops the RID
     handle without calling `rd->free()` (see `:957`-`:961`). That is a
     resource leak charged to the upstream owner that destroyed the
     manager first.

### StreamingGlobalAtlasRegistry

1. **Owner identity:** `class StreamingGlobalAtlasRegistry` at
   `modules/gaussian_splatting/core/streaming_global_atlas_registry.h:22`.
   Stored by value as a member of `GaussianStreamingSystem`
   (`global_atlas_registry` at `gaussian_streaming.h:83`).
2. **What it owns:**
   - `asset_meta_buffer` (RID) at
     `streaming_global_atlas_registry.h:88`.
   - `chunk_meta_buffer` (RID) at `:89`.
   - `asset_chunk_index_buffer` (RID) at `:90`.
   - Companion CPU-side mirrors `asset_meta_cpu` (`:94`), `chunk_meta_cpu`
     (`:95`), `asset_chunk_index_cpu` (`:96`) plus dirty-tracking vectors.
   - `global_atlas_state` (`GlobalAtlasState`, defined at `:12`) which
     republishes the same RIDs plus the atlas-wide
     `atlas_gaussian_buffer` and `quantization_buffer` for renderer
     consumption.
3. **Created where:** Buffers built by `sync_to_gpu()` (declared at `:48`),
   called from the streaming frame pipeline at
   `GS_STARTUP_SCOPE("streaming_atlas_sync_gpu")` at
   `gaussian_streaming.cpp:1318`.
4. **Destroyed where:** `cleanup(RenderingDevice *)` declared at
   `streaming_global_atlas_registry.h:42`. Called from
   `GaussianStreamingSystem`'s destructor at `gaussian_streaming.cpp:932`.
   **Idempotent:** Yes — frees each RID under an `.is_valid()` guard.
5. **Threading / locking:** Owned exclusively by `GaussianStreamingSystem`;
   no independent locking.
6. **Known leak vectors / TODOs:**
   - Same null-device leak vector as the parent system (see above) — when
     the parent's destructor cannot resolve `rd`, `cleanup(nullptr)` will
     drop the RID handles silently.

### GPUBufferManager

1. **Owner identity:** `class GPUBufferManager` (RefCounted) at
   `modules/gaussian_splatting/renderer/gpu_buffer_manager.h:10`. Field
   storage at `:98`-`:153`.
2. **What it owns:**
   - Double-buffered `BufferSet buffer_sets[BUFFER_COUNT]` at `:134` (with
     `BUFFER_COUNT = 2` at `:132`). Each `BufferSet` (`:100`) owns
     `gaussian_buffer`, `sort_key_buffer`, `sorted_indices_buffer`, and
     `fence` RIDs.
   - `uniform_buffer` (RID) at `:135`, plus `uniform_buffer_device` pointer
     at `:136`.
   - `DeferredDeletionQueue` (defined inline at `:29`) — note: the
     **renderer's** `ResourceState::deletion_queue` at
     `render_facade_state_types.h:51` is a separate instance of this type,
     owned by the renderer (not by the buffer manager).
3. **Created where:**
   - Constructor at `gpu_buffer_manager.cpp:38`. Initialized by
     `initialize(RenderingDevice *, uint32_t)` declared at
     `gpu_buffer_manager.h:179`. Buffers allocated via `create_buffers()`
     (`:154`) and `_create_buffer_set()` (`:156`).
   - Renderer construction site wrapped by
     `GS_STARTUP_SCOPE("gpu_buffer_manager_init")` at
     `render_resource_orchestrator.cpp:165`.
4. **Destroyed where:**
   - Destructor at `gpu_buffer_manager.cpp:42`. Calls `cleanup_buffers()`
     (`:155`) which iterates `_destroy_buffer_set()` per set.
   - Renderer drops the `Ref<>` at `gaussian_splat_renderer.cpp:1329`
     (`get_resource_state().buffer_manager.unref()`).
   - **Idempotent:** Yes — `_has_allocated_resources()` (`:158`) gates
     re-entry; `_reset_state()` (`:159`) clears handles.
5. **Threading / locking:** Single-owner on the render thread.
6. **Known leak vectors / TODOs:**
   - The `DeferredDeletionQueue::flush_all()` is invoked from the
     renderer at `gaussian_splat_renderer.cpp:1249` before sub-owners
     release their RIDs. Any RID queued for deferred deletion after this
     point will not be freed if the renderer is mid-teardown — this is
     the contract the lifetime-proof fixture must protect.

### GaussianMemoryStream

1. **Owner identity:** `class GaussianMemoryStream` (RefCounted) at
   `modules/gaussian_splatting/renderer/gpu_memory_stream.h:36`. Field
   storage at `:96`-`:130`.
2. **What it owns:**
   - `StreamBuffer buffers[BUFFER_COUNT]` at `gpu_memory_stream.h:98` with
     `BUFFER_COUNT = 3` at `:97` — each `StreamBuffer` (`:48`) owns one
     `gpu_buffer` RID and a `gpu_allocation_device` pointer (`:50`-`:51`).
   - `MemoryPool gpu_memory_pool` at `:110` — CPU-side suballocation
     bookkeeping over the GPU buffers.
   - Pool blocks (`MemoryPool::Block` at `:77`) carry offset/size/free state.
3. **Created where:** Constructor at `gpu_memory_stream.cpp:59`.
   `initialize()` at `:105` allocates the three stream buffers via
   `_create_buffer()` (`:183`).
4. **Destroyed where:**
   - Destructor at `gpu_memory_stream.cpp:63`. Renderer-side ownership
     drops at `gaussian_splat_renderer.cpp:1297`.
   - `shutdown()` at `gpu_memory_stream.cpp:164` is the explicit release
     path: `wait_for_all_uploads()` → `_destroy_buffer()` for each
     `BUFFER_COUNT` slot (`:170`-`:172`) → clears the memory pool
     (`:175`-`:177`) → nulls `rd` (`:179`).
   - **Idempotent:** Yes — `shutdown()` early-returns at `:165` when
     `rd` is already null.
5. **Threading / locking:** `StreamBuffer::state` is `std::atomic`; the
   ring is also tracked via `std::atomic<int>` write/read/upload indices at
   `:99`-`:102`. No internal mutex.
6. **Known leak vectors / TODOs:** None known beyond the manager-coupled
   destructor ordering noted under `GaussianStreamingSystem`.

### GPUCuller

1. **Owner identity:** `class GPUCuller` (RefCounted) at
   `modules/gaussian_splatting/interfaces/gpu_culler.h:32`. Field storage
   at `:265`-`:305`.
2. **What it owns:**
   - `shader` (RID) at `gpu_culler.h:267`,
     `frustum_shader_version` (`:268`), `pipeline` (`:269`).
   - Per-pass buffers: `param_buffer` (`:274`), `counter_buffer` (`:275`),
     `visible_buffer` (`:276`), `distance_buffer` (`:277`),
     `importance_buffer` (`:278`), `consolidated_buffer` (`:279`).
   - `instance_param_buffer` at `:280` and the
     `InstanceUniformSetCache instance_uniform_set_cache` (`:252`) at
     `:299` — caches `uniform_set` keyed on input RIDs.
   - `Ref<BatchedAsyncReadback> batched_readback` at `:305` — async-readback
     coalescer.
   - CPU staging buffers `param_bytes` (`:288`), `counter_bytes` (`:289`),
     `instance_param_bytes` (`:290`).
3. **Created where:**
   - Constructor at `gpu_culler.cpp:83`. `initialize()` at `:107` binds the
     `RenderingDevice` and instantiates the shader/pipeline via
     `_ensure_shader()` (declared at `:309`). Per-frame buffers grown by
     `_ensure_buffers()` (`:310`).
4. **Destroyed where:**
   - Destructor at `gpu_culler.cpp:103`.
   - `shutdown()` at `gpu_culler.cpp:143` is the explicit release path; the
     renderer calls it at `gaussian_splat_renderer.cpp:1304`. Releases
     buffers via `_release_resources()` (declared at `gpu_culler.h:314`)
     and invalidates `instance_uniform_set_cache` via
     `_invalidate_instance_uniform_set_cache()` (`:313`).
   - **Idempotent:** Yes — `bool initialized` at `:293` gates entry.
5. **Threading / locking:** Async readback path uses
   `BatchedAsyncReadback` which has its own internal synchronization;
   per-frame state is single-thread (render thread).
6. **Known leak vectors / TODOs:**
   - `instance_uniform_set_cache` is keyed by input RIDs; if upstream
     producers change buffers without calling
     `_invalidate_instance_uniform_set_cache()` the cached uniform set
     dangles on the wrong device. Currently only the cull path itself
     drives invalidation.

### GPUSorter (`IGPUSorter` implementations)

1. **Owner identity:** `class IGPUSorter` (RefCounted) at
   `modules/gaussian_splatting/renderer/gpu_sorter.h:149`. Concrete
   implementations `BitonicSort` (`:245`), `RadixSort`, `OneSweepSort`.
   Stored by `Ref<>` in `SortingState::gpu_sorter` at
   `render_state_types.h:115`.
2. **What it owns:** Per-algorithm, a shader + pipeline + scratch buffers.
   `BitonicSort` holds `bitonic_shader` and `bitonic_pipeline` (`gpu_sorter.h:257`-`:258`)
   plus `uniform_set`; `RadixSort` and `OneSweepSort` hold equivalent
   pass-specific buffers (chained-scan buffer for OneSweep).
3. **Created where:**
   - `BitonicSort::initialize()` at `gpu_sorter.cpp:556`,
     `RadixSort::initialize()` at `:2002`, `OneSweepSort::initialize()` at
     `:2675`. Variant selection wrapped by
     `GS_STARTUP_SCOPE("sorter_create_variant")` at `:1478`.
4. **Destroyed where:**
   - `BitonicSort::shutdown()` at `gpu_sorter.cpp:682`,
     `RadixSort::shutdown()` at `:2293`, `OneSweepSort::shutdown()` at
     `:3074`. Renderer calls `gpu_sorter->shutdown()` at
     `gaussian_splat_renderer.cpp:1334`.
   - **Idempotent:** Yes — `BitonicSort::shutdown()` (`:682`) validates the
     device against the current submission device and against
     `resource_device_generation` (see comment block at `:686`-`:693`)
     before calling `rd->free`; stale-device case zeros RIDs without
     freeing. `SAFE_FREE` macro at `:697` guards on `is_valid()`.
5. **Threading / locking:** Each sorter owns a `std::atomic<bool> is_sorting`
   at `gpu_sorter.h:156` so callers can detect in-flight async sort.
   Resource-device validation is delegated to
   `ResourceOwnerMismatchContract` (see ISSUE-010 referenced at `:689`).
6. **Known leak vectors / TODOs:**
   - The stale-device branch at `gpu_sorter.cpp:710`-onward zeros RIDs
     without freeing — by design, because the device they belong to is
     gone. PR 3's lifetime fixture should not flag this case but should
     verify it does not fire during a clean teardown.

### TileRenderer

1. **Owner identity:** `class TileRenderer` (RefCounted) at
   `modules/gaussian_splatting/renderer/tile_renderer.h`. Stored as
   `Ref<TileRenderer> TileRendererState::renderer` at
   `render_facade_state_types.h:130`.
2. **What it owns:** Composed of resource controllers declared in
   `renderer/tile_render_resources.h`:
   - `TileRenderTargets` (`tile_render_resources.h:35`) — `output_texture`,
     `depth_texture`, `normal_texture`, `output_texture_external`,
     `depth_texture_external`, `resolved_texture`,
     `resolved_depth_texture`, `tile_framebuffer` (`:49`-`:58`) and
     `tile_framebuffer_format` (`:59`).
   - `TileResourceController` (`:76`) — caches `tracked_color_output` and
     `tracked_depth_output` RIDs plus a `RenderDeviceManager *` pointer
     (`:95`) used to forget tracked resources on shutdown.
   - `TileShaderResources` (`:107`) — `tile_binning_shader/pipeline`,
     `tile_binning_count_shader/pipeline`, `tile_prefix_shader[_pass2/3]`
     and pipelines, `tile_raster_shader/pipeline`,
     `tile_raster_compute_shader/pipeline`, `tile_resolve_shader/pipeline`
     plus the `*_shader_source` GLSL-text owners (`:115`-`:141`).
   - `TileGlobalSortResources` (`:154`) — `Ref<IGPUSorter> sorter` (`:162`),
     `keys_buffer` (`:168`), `values_buffer` (`:169`),
     `tile_counts_buffers[2]` (`:170`), `tile_ranges_buffer`,
     `prefix_total_buffer`, `indirect_dispatch_buffer`, `wg_sums_buffer`,
     `wg_offsets_buffer` (`:174`-`:178`).
   - `TileUniformBuffers` (`:200`) — `param_uniform_buffer` (`:210`),
     `prefix_param_uniform_buffer` (`:212`),
     `default_state_uniform_buffer` (`:214`).
   - `TileProjectionBuffers` (`:218`) — `projection_buffer` (`:226`).
   - `TileSHCacheBuffers` (`:234`) — `sh_color_cache` (`:242`).
   - `TileSubpixelHistoryBuffers` (`:248`) — `subpixel_history_buffer` (`:256`).
   - `TileSubpixelVisibilityBuffers` (`:261`) — `subpixel_visibility_buffer` (`:269`).
3. **Created where:**
   - Constructor at `tile_renderer.cpp:1301`.
   - `initialize()` at `tile_renderer.cpp:1440`: compiles shaders via
     `_compile_tile_shaders()` (line `:1462`), allocates output textures
     via `_ensure_resources()` (`:1469`), eager-creates the graphics
     raster pipeline (post-`_ensure_resources` block at `:1480`+ — see
     `GS_STARTUP_SCOPE("first_frame_raster_pipeline_create")` at
     `tile_render_rasterizer_stage.cpp:172` for the lazy fallback that
     this eager path replaces).
4. **Destroyed where:**
   - Destructor at `tile_renderer.cpp:1431`: clears adaptive-overlap
     runtime state, unregisters from performance monitors, then calls
     `cleanup()` (line `:1437`).
   - `cleanup()` at `tile_renderer.cpp:1611`: in order, calls
     `clear_output_resource_tracking()` (`:1612`), then on each resource
     controller `release(device)`:
     `projection_buffers.release` (`:1617`),
     `sh_cache_buffers.release` (`:1618`),
     `subpixel_history_buffers.release` (`:1619`),
     `subpixel_visibility_buffers.release` (`:1620`),
     `debug_stats.free_buffers` (`:1621`),
     `global_sort_resources.release` (`:1622`),
     `shader_resources.release(device, pipeline_owner)` (`:1624`),
     `render_targets.tile_framebuffer` (`:1625`-`:1631`),
     resolve_stage samplers (`:1632`-`:1651`),
     `resolve_stage.free_fallback_lighting_buffers` (`:1652`),
     `uniform_buffers.release` (`:1653`).
   - Renderer drives cleanup at `gaussian_splat_renderer.cpp:1284`.
   - **Idempotent:** Yes — each release path checks `device` validity and
     each resource controller's `release()` is implemented with
     `is_valid()` guards. Re-entry no-ops.
5. **Threading / locking:** Single-owner; relies on the renderer's
   render-thread dispatch ordering. `_get_resource_device()` (called at
   `:1614`) may return null if the manager singleton is already gone,
   which falls through to `uniform_buffers.reset_state()` at `:1655`
   (drops handles without freeing).
6. **Known leak vectors / TODOs:**
   - The null-device fallback at `:1654`-`:1656` is a structural leak
     vector when ordering is wrong (manager shut down before renderer).
     PR 3 should assert this branch is not taken on the happy path.

### GaussianSplatNode3D

1. **Owner identity:** `class GaussianSplatNode3D` (Node3D) at
   `modules/gaussian_splatting/nodes/gaussian_splat_node_3d.h`. Field
   storage at `:200`-`:248`.
2. **What it owns:**
   - `Ref<GaussianSplatRenderer> renderer` at
     `gaussian_splat_node_3d.h:230` and `Ref<GaussianData> renderer_data`
     at `:231` — the strong refs that propagate into the director's
     SharedWorld.
   - `render_instance` (RID, RenderingServer instance) at `:207`.
   - `gaussian_base` (RID, RenderingServer base) at `:208`.
   - `cached_viewport_render_target` (`:220`),
     `cached_viewport_render_texture` (`:221`),
     `Ref<ViewportTexture> cached_viewport_texture` (`:222`) — viewport
     bookkeeping for editor and runtime preview.
   - `Ref<ColorGradingResource> color_grading` at `:177`.
   - `Ref<GaussianSplatAsset> splat_asset` (`:131`) and
     `runtime_asset` (`:132`).
3. **Created where:**
   - `_notification_enter_tree()` at `gaussian_splat_node_3d.cpp:349`:
     registers with `GaussianSplatManager::register_node()` (line `:357`)
     and triggers `_ensure_renderer()` + `_update_render_instance()` +
     `_register_shared_renderer()` (`:371`-`:373`).
   - `_notification_enter_world()` at `:377`: same enter sequence after
     world attach.
4. **Destroyed where:**
   - Destructor at `gaussian_splat_node_3d.cpp:338`:
     `_disconnect_viewport_observers()` → `_clear_asset()` →
     `_release_gaussian_base()` → `_clear_parent_visibility_tracking()` →
     frees `render_instance` via `RS::free()` (line `:345`).
   - `_notification_exit_tree()` at `:387` is the scene-attached release
     path: destroys debug HUD control, calls
     `renderer_helper.release_renderer_settings_ownership()` (`:393`),
     `_unregister_shared_renderer()` (`:394`),
     `_release_gaussian_base()` (`:407`), frees `render_instance` (`:409`-`:412`),
     and calls `manager->unregister_node(this)` (`:415`).
   - **Idempotent:** `is_valid()` guards on `render_instance`; the
     destructor and `_notification_exit_tree()` share the same release
     calls and tolerate prior release.
5. **Threading / locking:** Main thread only (Godot scene-tree contract).
   The `GaussianSplatManager::register_node()` path under
   `active_nodes_mutex` (manager Level 3 lock).
6. **Known leak vectors / TODOs:**
   - When `_exit_tree` runs but the node is later re-attached, the
     renderer ref is intentionally retained — see the comment block at
     `gaussian_splat_node_3d.cpp:396`-`:403` explaining why
     `grading_pushed_for_current_data` is not reset here. This is a
     deliberate sticky reference into the director-owned SharedWorld;
     PR 4 should preserve this semantic.

## Known leak vectors

- **F6 reload leak** — `modules/gaussian_splatting/core/gaussian_splat_scene_director.cpp:351`
  (and the surrounding comment block at `:349`-`:352`). If the director
  singleton is not destroyed at module finalization, every `SharedWorld`
  retains its `Ref<GaussianSplatRenderer>` and the entire renderer GPU
  graph leaks. **PR 4 (explicit SharedWorld teardown contract) formalizes
  this.**
- **Failed-init crash cascade (602 events)** — When
  `GaussianSplatRenderer::_initialize_on_render_thread()` (declared at
  `gaussian_splat_renderer.h:661`) fails partway through sub-owner
  instantiation, `_teardown_resources()` (`gaussian_splat_renderer.cpp:1242`)
  must be invariant under partially-constructed state. The
  `teardown_resources_started` guard at `gaussian_splat_renderer.h:624`
  protects against double-entry, but each sub-owner's `shutdown()` must
  itself be safe when never `initialize()`d. **PR 1 hardens this; PR 3
  proves it by instrumenting every owner with a lifetime counter.**
- **OutputCompositor framebuffer-format cache unbounded** —
  `cached_framebuffers` at `interfaces/output_compositor.h:115` and the
  parallel `framebuffer_validation_cache` at `:116`. No LRU; editor
  viewport reformat and device-switch cycles accumulate entries
  indefinitely. The scratch pool comment at `:216`-`:219` documents the
  exact reasoning that should be applied here. **PR 5 (OutputCompositor
  LRU bound).**
- **Orphan StringNames at exit** — `GSStartupTraceScope::phase` (a
  `StringName` field at `modules/gaussian_splatting/logger/startup_trace.h:90`)
  is constructed from a string literal at each `GS_STARTUP_SCOPE(name)`
  call site (`:103`). At process exit these StringNames are not
  explicitly released. Known sites (from `register_types.cpp:78` and
  the smoke-test capture):
  - `"module_register"` — `modules/gaussian_splatting/register_types.cpp:78`.
  - `"manager_construct"` — `modules/gaussian_splatting/core/gaussian_splat_manager.cpp:219`.
  - `"device_request_primary"` — `modules/gaussian_splatting/core/gaussian_splat_manager.cpp:255`.
  - `"device_request_shared"` — `modules/gaussian_splatting/core/gaussian_splat_manager.cpp:259`.
  - `"renderer_construct"` — `modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp:871`.
  - `"gpu_buffer_manager_init"` — `modules/gaussian_splatting/renderer/render_resource_orchestrator.cpp:165`.
  - `"sorter_create_variant"` — `modules/gaussian_splatting/renderer/gpu_sorter.cpp:1478`.
  - `"first_frame_raster_pipeline_create"` — `modules/gaussian_splatting/renderer/tile_render_rasterizer_stage.cpp:172`.
  - `"shader_compile_binning"` / `"shader_compile_prefix"` /
    `"shader_compile_raster"` — `modules/gaussian_splatting/renderer/shader_compilation_helper.cpp:384`, `:423`, `:476`.
  - `"streaming_persistent_buffer_alloc"` / `"streaming_atlas_build_cpu"` /
    `"streaming_atlas_sync_gpu"` —
    `modules/gaussian_splatting/core/gaussian_streaming.cpp:1266`, `:1314`, `:1318`.
  - `"asset_populate_gaussian_data"` /
    `"asset_prefetch_parallel"` —
    `modules/gaussian_splatting/core/gaussian_splat_asset.cpp:1570`, `:1511`.
  - `"ply_payload_parse"` —
    `modules/gaussian_splatting/io/ply_loader.cpp:176`.
  **PR 6 (orphan StringName cleanup) releases these.**
- **Null-device destruction races** — `GaussianStreamingSystem::~GaussianStreamingSystem`
  (`modules/gaussian_splatting/core/gaussian_streaming.cpp:921`),
  `StreamingGlobalAtlasRegistry::cleanup`, and `TileRenderer::cleanup`
  (`modules/gaussian_splatting/renderer/tile_renderer.cpp:1611`) all
  tolerate a null `RenderingDevice` and drop RID handles silently. This
  is benign when the manager has already destroyed the device, but it
  becomes a leak when ordering inverts (renderer destroyed before
  manager-shutdown completes). The fixture in PR 3 should detect
  inverted ordering by asserting `rd != nullptr` on each release path.

## Thread-safety contracts

- **`GaussianSplatManager` L1-L4 lock hierarchy.** The canonical ordering
  rules live in the header comment at
  `modules/gaussian_splatting/core/gaussian_splat_manager.h:54-71`:
  - Level 1 `submission_mutex` (static, outermost) — serializes GPU
    submission. Public constants `LOCK_LEVEL_SUBMISSION = 1` etc. at
    `:82`-`:85`.
  - Level 2 `resource_maps_mutex` — protects `gaussian_buffers` /
    `buffer_lookup` / `dynamic_asset_cache` and the two `*_owner_devices`
    maps.
  - Level 3 `active_nodes_mutex` — protects the registered-node set.
  - Level 4 `local_device_destroy_request_mutex` /
    `local_device_destroy_pending_mutex` (innermost) — local-device
    teardown dispatch and pending-device handoff.

  Rules: never acquire a lower-numbered lock while holding a
  higher-numbered one. In practice no method currently nests two of
  these locks in the same scope. The destructor at
  `gaussian_splat_manager.cpp:666` enforces the sequential rule by
  releasing each lock before acquiring the next. DEV_ENABLED builds
  runtime-validate via `_gs_lock_level_guard`.

- **`GaussianSplatSceneDirector::world_mutex`** at
  `core/gaussian_splat_scene_director.h:355` guards every read and write
  of `worlds`. The director does not call into the manager's L1-L4
  hierarchy while holding `world_mutex`.

- **Renderer teardown idempotency** is enforced by
  `std::atomic<bool> teardown_resources_started` at
  `renderer/gaussian_splat_renderer.h:624`, flipped via
  `compare_exchange_strong` at `gaussian_splat_renderer.cpp:1244`. This is
  the primary contract any new teardown caller (e.g. the lifetime-proof
  fixture in PR 3) must respect.

- **Render-thread dispatch** is brokered by `IRenderThreadDispatcher`
  (`interfaces/render_thread_dispatcher.h:10`-`:25`); state at
  `:44`-`:51`. Renderer destructor at `gaussian_splat_renderer.cpp:1233`
  uses `_dispatch_call_on_render_thread_blocking()` to route
  `_teardown_on_render_thread()` to the render thread when possible,
  with a synchronous fallback at `:1239` when dispatch is rejected.

- **Per-sub-owner locking** is documented per owner above. The pattern is
  consistent: sub-owners do not introduce process-global locks; they
  either rely on the renderer's render-thread dispatch ordering or use
  `std::atomic` for ring-buffer indices and async-readback flags.

## Cross-references

- [`docs/architecture/stage-first-ownership-inventory.md`](stage-first-ownership-inventory.md)
  — stage-level write-ownership for #356 (W0 characterization). Read
  alongside this doc for the W0→W3 decomposition work.
- [`modules/gaussian_splatting/ARCHITECTURE.md`](../../modules/gaussian_splatting/ARCHITECTURE.md)
  — subsystem map and data flow.
- [`modules/gaussian_splatting/MEMORY_SUBSYSTEM.md`](../../modules/gaussian_splatting/MEMORY_SUBSYSTEM.md)
  — VRAM budget policy and persistent-buffer right-sizing (PR #344).
- [`modules/gaussian_splatting/READING_ORDER.md`](../../modules/gaussian_splatting/READING_ORDER.md)
  — recommended reading sequence for new contributors.
- Related work-package issues / PRs:
  - **#352** — Renderer Lifetime Proof (this work package).
  - **#327** — earlier teardown-ordering hardening.
  - **#298** — RID leak inventory (predecessor inventory effort).
  - **#316** — render-thread dispatch contract.
  - PRs 1, 3, 4, 5, 6 of #352 will land alongside this doc; PR numbers
    to be filled in once assigned.

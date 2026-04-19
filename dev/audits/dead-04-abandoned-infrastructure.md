# Abandoned Infrastructure — Deep Sweep

## Summary

Seventeen abandoned (or partially abandoned) subsystems catalogued in the Gaussian Splatting Godot module. Two of the seed findings were already partially repaired — residual carcass remains and is documented. Most damning finds: `ClusterCuller` (555 LOC + 175-line GLSL shader) is built, shader-compiled, GDScript-registered, and instantiated nowhere in C++; `GaussianAnimationStateMachine` (965 LOC) is plumbed through save/load and tests, instantiated in production nowhere; LOD `ChunkLODMetadata` / `LODDebugStats` exist as standalone schemas with no consumer; thirteen `I*` interfaces each have exactly one implementation (pure code tax); and `RuntimePorts` PMF dependency-injection proxies cover only 60/134 (~45%) of orchestrator→renderer calls — the other 74 calls bypass the port, so the DI is a partial sieve rather than a real seam. All call-site counts in this report are based on literal grep counts in the tree, excluding `tests/`, `docs/`, and `doc_classes/` unless noted.

## Method

1. Read each seed finding, re-grep its symbols with the lens "is it *branched on* in production, or merely read/logged?" Several seeds turned out to have had wiring added since the seed was drafted — the abandonment had moved downstream (reasons still produced but discarded).
2. For each subsystem, counted production call-sites excluding `tests/` and `doc_classes/`.
3. For each *interface* file (`I*`), located its implementations and scored the count.
4. Inspected the `.h` file for any class with a registered `ProjectSettings` schema; grepped the codebase for readers of each setting.
5. Followed the PMF (`runtime_ports` pattern) in every renderer orchestrator and compared direct `renderer->` calls to PMF-mediated calls.
6. Spot-checked register_types.cpp for `GDREGISTER_CLASS` entries whose C++ implementations have no internal instantiation.

Evidence cited throughout is `file:line` from the verification runs. Inline-header `static` helpers are considered "called" only when the call is from a production TU, never from a test or from the same header's declaration block.

## Catalogue

### 1. ClusterCuller subsystem (LiteGS-style coarse culler)

**Design quality:** Good. Separate compute shader (`cluster_cull.glsl`, 175 LOC), Morton-code clustering (`ClusterBuilder`, 326 LOC), indirect dispatch contract shared in `pipeline_io_contracts.h::ClusterCullIndirectDispatchLayout`, capability gate, async readback, GDScript-exposed stats. Textbook hierarchical coarse-culling layer.

**Location:**
- `modules/gaussian_splatting/interfaces/cluster_culler.h` (219 LOC)
- `modules/gaussian_splatting/interfaces/cluster_culler.cpp` (555 LOC)
- `modules/gaussian_splatting/lod/cluster_builder.h` (153 LOC)
- `modules/gaussian_splatting/lod/cluster_builder.cpp` (326 LOC)
- `modules/gaussian_splatting/compute/cluster_cull.glsl` (175 LOC)

**Size:** ~1,430 LOC including the GLSL + shader generator entries in `shaders/compile_shaders.py:494` and `:570`.

**Production consumers:** **Zero C++ instantiations.** Single reference is `Ref<ClusterCuller> cluster_culler;` declared at `modules/gaussian_splatting/interfaces/gpu_culler.h:318`. `grep 'cluster_culler' modules/gaussian_splatting/interfaces/gpu_culler.cpp` returns nothing — never assigned, never called. Class is registered in `register_types.cpp:135` for GDScript, so the C++ storage byte is consumed but the functionality is never engaged.

**Test consumers:** 0 direct (`ClusterCuller` is not tested; `ClusterBuilder::build_clusters` is exercised only via the unused `ClusterCuller::build_clusters` path, and even that path has no active caller).

**Ad-hoc replacement in production (if any):** Coarse culling is ad-hoc — `GPUCuller` performs per-chunk frustum culling directly (`interfaces/gpu_culler.cpp`) without cluster pre-pass. The LiteGS-style cluster layer was never integrated.

**Verdict:** **DELETE.** The whole ClusterCuller + GLSL + ClusterBuilder + pipeline-io cluster layout contract is dormant. ~1,430 LOC + one shader module + one line of dead storage in GPUCuller. If you want the feature, re-enable is a 2-day integration (wire `gpu_culler->cluster_culler.instantiate()` in GPUCuller::initialize, call `build_clusters` on atlas ingest, call `cull_clusters` pre-chunk-frustum-pass). But it was clearly aborted mid-landing.

**Effort to resolve:** 4 hours to delete. 2 days to actually wire into `GPUCuller::apply_cull_settings`. Recommendation: delete; reintroduce only if a benchmark justifies it.

---

### 2. GaussianAnimationStateMachine (animated splats)

**Design quality:** Complete. Keyframe interpolator, clip management, state transitions, metadata changes, incremental save hooks. Plumbed into the binary persistence format (`GaussianSceneSerializer::_write_animation_data_chunk`, `_read_animation_data_chunk`).

**Location:**
- `modules/gaussian_splatting/animation/animation_state_machine.cpp` (965 LOC)
- `modules/gaussian_splatting/animation/animation_state_machine.h` (184 LOC)
- `modules/gaussian_splatting/animation/keyframe_interpolator.cpp` (227 LOC)
- `modules/gaussian_splatting/animation/keyframe_interpolator.h` (70 LOC)
- `modules/gaussian_splatting/core/gaussian_data_animation.cpp` (pool-adjacent)

**Size:** ~1,450 LOC in `animation/` alone. Serialization touches another ~300 LOC across `persistence/`.

**Production consumers:** Zero. All 8 `GaussianAnimationStateMachine` instantiations in the tree are in `tests/test_animation_interpolation.h`. No renderer, no scene director, no manager, no node ever calls `state_machine.tick()` or constructs one. Every persistence function accepts it as an optional `nullptr` parameter (`save_scene(... animation = nullptr, ...)` at `persistence/gaussian_scene_serializer.h:146`) — but see #16: the persistence layer itself has no production callers, so the animation parameter is never populated in practice.

**Test consumers:** 8 (`tests/test_animation_interpolation.h:10, 30, 45, 71, 94, 118, 142, 155`).

**Ad-hoc replacement in production (if any):** None — animated Gaussian splats are not a supported runtime feature. The module ships a feature nobody drives.

**Verdict:** **KEEP-DORMANT with scoped quarantine** (user may be using it from GDScript), OR **DELETE with explicit feature-scope decision.** This is an architectural decision, not a cleanup — the file format reserves chunk IDs for animation. If GDScript users of the Godot build are consuming it, it's public API; if not, it's pure code tax.

**Effort to resolve:** 1 hour to audit GDScript exposure; 4 hours to rip out the module + serializer chunks if unused.

---

### 3. LOD: `ChunkLODMetadata` + `LODDebugStats`

**Design quality:** Well-specified. Per-chunk LOD metadata (skip factor, SH band, opacity multiplier, needs_update flag) plus a debug stats aggregator with histogram output.

**Location:**
- `modules/gaussian_splatting/lod/lod_config.h:137` (`ChunkLODMetadata`)
- `modules/gaussian_splatting/lod/lod_config.h:152` (`LODDebugStats`)
- `modules/gaussian_splatting/lod/lod_config.cpp:233, :247, :263, :302` (impls)

**Size:** ~80 LOC of struct + 70 LOC of impl.

**Production consumers:** **Zero.** `grep 'ChunkLODMetadata'` returns only the header, the `lod_config.cpp` impl, and `test_config_validation.h`. `LODDebugStats::update_from_chunks` is called nowhere in production. Actual LOD decisions happen in `core/streaming_visibility_controller`, `core/gaussian_splat_scene_director`, `core/gaussian_streaming`, which reach directly into `LODConfig::calculate_lod_level` and maintain their own per-chunk state — they do **not** use the `ChunkLODMetadata` schema.

**Test consumers:** 0 (the `test_config_validation.h` hit is for the LODConfig, not the metadata struct.)

**Ad-hoc replacement in production (if any):** Each consumer stores its own LOD derivatives inline on `StreamingChunk` or scene-level structures; the library struct is bypassed.

**Verdict:** **DELETE** `ChunkLODMetadata` and `LODDebugStats` (~150 LOC gone). Keep `LODConfig` and its helpers (`calculate_lod_level`, etc.) — those are consumed.

**Effort to resolve:** 1 hour.

---

### 4. Painterly pass graph

**Design quality:** Clean PassNode/PassType/TextureSlot abstraction with enabled flag per pass and version bump on change. Classic render-graph skeleton.

**Location:**
- `modules/gaussian_splatting/renderer/painterly_pass_graph.h:31-93` (`PassNode`, `get_passes`, `find_pass`)
- `modules/gaussian_splatting/renderer/painterly_pass_graph.cpp` (352 LOC)

**Size:** 448 LOC.

**Production consumers:** Zero. `get_passes()` at `painterly_pass_graph.h:90` — **no callers.** `find_pass()` declared `:91`, defined at `painterly_pass_graph.cpp:126` — **no callers.** The `passes` vector is populated in `_rebuild_passes` and never read. The texture-slot machinery is used, but the node-graph introspection surface is dead.

**Test consumers:** 0.

**Ad-hoc replacement in production (if any):** `PainterlyRenderer::execute_passes` (in `interfaces/painterly_renderer.cpp`) hard-codes sobel+brush dispatch sequence — ignores the graph entirely.

**Verdict:** **DELETE `passes` vector + `PassNode` struct + `get_passes`/`find_pass` + `_rebuild_passes`.** Keep `TextureSlot`, `TextureInfo`, resize/reset machinery — those are used.

**Effort to resolve:** 2 hours. Risk: trivial — tools never touch the removed surface.

---

### 5. RuntimePorts PMF dependency-injection (orchestrator-wide)

**Design quality:** Member-pointer based dependency injection: each orchestrator declares a nested `RuntimePorts` struct of `T (Renderer::*)(...)` pointers, defaulted to the real methods. Allows test swap-in.

**Location:** `RuntimePorts` declared in 8 orchestrator headers:
- `renderer/render_streaming_orchestrator.h:16-34` (8 pointers)
- `renderer/render_resource_orchestrator.h`
- `renderer/render_quality_orchestrator.h`
- `renderer/render_output_orchestrator.h`
- `renderer/render_diagnostics_orchestrator.h`
- `renderer/render_debug_state_orchestrator.h`
- `renderer/render_data_orchestrator.h`
- `renderer/render_config_orchestrator.h`

**Size:** ~40 LOC per header × 8 = ~320 LOC of PMF declarations.

**Production consumers (PMF-mediated vs direct calls in orchestrator `.cpp` files):**
```
                                    PMF   direct-bypass   total
render_data_orchestrator.cpp:         1    3               4
render_config_orchestrator.cpp:       2    2               4
render_debug_state_orchestrator.cpp:  2   15              17
render_diagnostics_orchestrator.cpp:  4    2               6
render_output_orchestrator.cpp:      11    3              14
render_resource_orchestrator.cpp:    15    9              24
render_quality_orchestrator.cpp:      6    2               8
render_streaming_orchestrator.cpp:   19   38              57
                                     ---  ---            ---
TOTAL:                                60   74             134
```
(So PMF mediates 60/134 ≈ 45% of orchestrator→renderer calls; 74 calls bypass the port.)

**Test consumers:** 0 (nobody overrides the defaulted PMFs in tests either — every `RuntimePorts` uses the defaulted member-pointers).

**Ad-hoc replacement in production (if any):** The `.cpp` bodies freely use `renderer->direct_method_call(...)` for 55% of the calls. The 60 PMF-mediated calls are a shotgun-shell of partial decoupling — the DI is not a clean seam, it's eight leaky sieves.

**Verdict:** **DELETE `RuntimePorts`** in all 8 orchestrators. Either (a) the orchestrators genuinely need decoupling — in which case the 74 bypassing calls need to route through ports too — or (b) they don't, which is what the existing call ratio proves. No test ever exploits the seam. The PMF-based indirection has runtime cost, compile-time cost (MSVC PMF forward-decl ODR issues — see memory `project_msvc_pmf_odr_trap.md`), and zero observed benefit.

**Effort to resolve:** 1 day. Rip the PMF structs, replace `(renderer->*runtime_ports.foo)(args)` with `renderer->foo(args)` globally. Regression risk: zero (identity transformation).

---

### 6. Compute fallback Route enum (4-way decision discarded to 1-way string)

**Design quality:** Clear four-value `FallbackDecision::Route` enum (`NONE`, `RETRY_NEXT_FRAME`, `USE_CPU_FALLBACK`, `DISABLE_STAGE`) with `CapabilityGatePolicy` inputs and `resolve_fallback()` decision function.

**Location:**
- `modules/gaussian_splatting/compute/compute_infrastructure.h:42-47, :87` (decl)
- `modules/gaussian_splatting/compute/compute_infrastructure.cpp:138-162` (impl)

**Size:** ~50 LOC for the decision layer.

**Production consumers:** 2 call sites — both treat the route as a log string, never branch on it.
- `modules/gaussian_splatting/interfaces/cluster_culler.cpp:83-87` — `fallback.route` formatted into `last_compute_error` via `fallback_route_name(fallback.route)`. Return value is `capability.to_error()` — route itself ignored.
- `modules/gaussian_splatting/interfaces/gpu_sorting_pipeline.cpp:221-225` — identical pattern: `vformat(" fallback=%s", ... fallback_route_name(fallback.route))`, return is the same error.

**Test consumers:** 4 (`tests/test_compute_infrastructure.h:82, 97, 111, 126` — they validate `decision.route` distinguishes routes, but no production code does).

**Ad-hoc replacement in production (if any):** There is no CPU fallback, no retry-next-frame logic, no disable-stage gate. When a compute stage fails, the caller errors and the frame skips. All four `Route` values produce identical behavior in production.

**Verdict:** **ENABLE** (implement the routes) or **DELETE** (collapse to a bool). Given RETRY_NEXT_FRAME and DISABLE_STAGE have real utility for shader-validation / capability-gate failures, **ENABLE** is the better call. The wiring plan: in each caller, switch on `fallback.route` — `RETRY_NEXT_FRAME` marks the stage as skip-this-frame with a timestamp, `DISABLE_STAGE` flips a persistent flag in `SubsystemState`, `USE_CPU_FALLBACK` invokes the CPU path where one exists (gpu_sorter has a CPU path, cluster_culler doesn't).

**Effort to resolve:** 2 days (enable) or 2 hours (collapse to `bool`).

---

### 7. `StageValidationHarness::all_valid` / `summarize_failures`

**Design quality:** Standard validation-collector pattern.

**Location:**
- `modules/gaussian_splatting/compute/compute_infrastructure.h:75-83`
- `modules/gaussian_splatting/compute/compute_infrastructure.cpp:96-118`

**Production consumers:** 2 instantiations but **each immediately discards the aggregation API**. `cluster_culler.cpp:461` and `gpu_sorting_pipeline.cpp:2428` both call `harness.validate(input)` once and inspect the returned `StageResult` inline — never calling `all_valid()` or `summarize_failures()`. The harness instance is used as a one-shot stage-runner, not as a validation accumulator.

**Test consumers:** 1 (`tests/test_compute_infrastructure.h:30, 31` exercise `all_valid`/`summarize_failures`).

**Ad-hoc replacement in production (if any):** Each call site tracks errors manually in `last_compute_error`.

**Verdict:** **DELETE** `records` vector, `all_valid()`, `summarize_failures()` from `StageValidationHarness`. Keep the single-shot `validate()` — but then the class is a glorified free function; merge it.

**Effort to resolve:** 2 hours.

---

### 8. `record_total_eviction` — orphan eviction counter path

**Design quality:** The split-counter design is correct (vis vs nonvis counters drive pressure/thrashing signals).

**Location:**
- `modules/gaussian_splatting/core/streaming_eviction_controller.cpp:42-44` (impl)
- `modules/gaussian_splatting/core/streaming_eviction_controller.h:29` (decl)
- `modules/gaussian_splatting/core/streaming_atlas.cpp:124` (single caller)

**Production consumers:** 1 — `streaming_atlas.cpp:124` inside `_unload_chunk` atlas-path bulk unload.

**Test consumers:** 0.

**Ad-hoc replacement in production (if any):** No — the bug *is* the replacement. `record_eviction_result` correctly splits vis/nonvis on the regular eviction path; atlas-path evictions bypass that split and bump `chunks_evicted_this_frame` only. Any pressure signal that consumes `visible_chunks_evicted_this_frame` is under-counted.

**Verdict:** **DELETE `record_total_eviction`, replace its one call-site with a proper classified call.** The fix is to determine whether the evicted chunk was visible (via `chunk.is_visible` on the `StreamingChunk` before the `_unload_chunk` call) and route to `record_eviction_result(EvictionResult::EvictedVisible|EvictedNonVisible)`.

**Effort to resolve:** 2 hours including test for visible-atlas eviction pressure signal.

---

### 9. `PipelineFeatureSet::enable_two_stage_sort`

**Design quality:** One of 5 boolean feature flags with tier-config defaults, provenance tracking, validator, project-settings registration, snapshot API — all the scaffolding a real feature flag deserves.

**Location:**
- `modules/gaussian_splatting/renderer/pipeline_feature_set.h:16`
- `modules/gaussian_splatting/renderer/pipeline_feature_set.cpp:52, 111, 126, 138, 154, 211, 234-236, 320`

**Size:** ~50 LOC for the two-stage-sort branch, plus its share of the ~344-LOC pipeline_feature_set.cpp.

**Production consumers:** Zero **behavioral** consumers. The flag is:
- Read from `ProjectSettings` at `.cpp:52`
- Promoted in `enable_all_experimental` at `:211`
- Disabled when `!p_global_sort_enabled` at `:234-236`
- Logged at `:320`
- Serialized into snapshot at `:111`
- Saved back at `:138`
- Reset to default at `:154`

…but **no `if (…enable_two_stage_sort)` in any pipeline/renderer code.** `grep 'enable_two_stage_sort' modules/gaussian_splatting/renderer/` returns only the config file itself. `tile_renderer.cpp`, `render_pipeline_stages.cpp`, `tile_render_*.cpp` all read other flags (`enable_packed_stage_data`, `enable_tighter_bounds`, `enable_sh_amortization`, `enable_fast_raster`) — never `enable_two_stage_sort`.

**Test consumers:** 0 (behaviorally — `test_config_validation.h` validates presence in snapshot, not behavior).

**Ad-hoc replacement in production (if any):** None — two-stage sort is simply not implemented; there is only the one-stage global sort and the per-instance streaming pipeline.

**Verdict:** **DELETE** `enable_two_stage_sort` from the feature set, quality-tier config, project-settings registration, and the provenance snapshot. ~30 LOC gone. Users with the project setting persisted will receive the "unknown setting" warning once, which is the correct behavior for retired flags.

**Effort to resolve:** 2 hours.

---

### 10. `GaussianRenderingDiagnostics` anomaly dump (off-by-default)

**Design quality:** Production-ready: pipeline trace, splat audit, cull guardrails, anomaly dump. Each has a typed setter/getter, GDScript binding, and default=false.

**Location:**
- `modules/gaussian_splatting/renderer/gaussian_splat_renderer.h:1300-1307`
- `modules/gaussian_splatting/renderer/render_debug_state_orchestrator.cpp:1083-1111` (setters)
- `modules/gaussian_splatting/doc_classes/GaussianSplatRenderer.xml:812-858` (documented)

**Production consumers:** All flags default to `false`, even in dev builds. `_default_pipeline_trace_enabled()` at `render_debug_state_orchestrator.cpp:32` returns `false` unconditionally (no `#ifdef DEBUG_ENABLED` switch).

**Test consumers:** Limited.

**Ad-hoc replacement in production (if any):** Debugging splat-anomalies falls back to GDScript toggling. When a user files a bug without knowing the flags exist, they ship default-off logs to the maintainer.

**Verdict:** **ENABLE by default in `DEBUG_ENABLED` builds.** `#ifdef DEBUG_ENABLED` in `_default_pipeline_trace_enabled` → `true`. 3-line fix. Caveat: measure perf cost in dev — the seed finding calls them "expensive-but-useful." If any single flag has unacceptable dev-mode cost, keep it off, but the cheap ones should be on.

**Effort to resolve:** 4 hours (1 hour code, 3 hours perf measurement).

---

### 11. `I*` interfaces with exactly-one implementation

**Design quality:** Textbook "program to an interface" — except each interface has exactly one implementation, so the interface is a pure cost without the abstraction benefit.

**Location:** `modules/gaussian_splatting/interfaces/renderer_interfaces.h` plus eight sibling files. Single-implementation interfaces:
- `IRenderer` (+ sub-interfaces `IRendererLifecycle`, `IRendererConfig`, `IRendererDebug`, `IRendererPipeline`) — impl `GaussianSplatRenderer` only
- `ICuller` / `IHierarchicalCuller` — impl `GPUCuller` only; `IHierarchicalCuller` has zero direct implementations
- `IRasterizer` — impl `TileRasterizer` only
- `IGPUSortingPipeline` — impl `GPUSortingPipeline` only
- `IInteractiveStateManager` — impl `InteractiveStateManager` only
- `IOverflowAutoTuner` — impl `OverflowAutoTuner` only
- `IPainterlyMaterialManager` — impl `PainterlyMaterialManager` only
- `IOutputCompositor` — impl `OutputCompositor` only
- `IPainterlyRenderer` — impl `PainterlyRenderer` only
- `IRenderDeviceManager` — impl `RenderDeviceManager` only
- `IRenderThreadDispatcher` — impl `RenderThreadDispatcher` only
- `ISyncPolicy` — impl `CoarseSyncPolicy` only (note the name implying a `Fine` was planned)
- `IDebugOverlaySystem` — impl `DebugOverlaySystem` only

**Counter-example (genuinely useful):** `IGPUSorter` has three real impls (`BitonicSort`, `RadixSort`, `OneSweepSort`) — keep.

**Size:** `renderer_interfaces.h` alone = 266 LOC of pure virtual methods; aggregate across 13 dead interfaces ≈ 800-1,000 LOC of vtable and declaration tax.

**Production consumers:** Each interface has exactly one user, which is its one implementation plus any field that holds a `Ref<I*>` — typically the renderer.

**Test consumers:** One (`tests/test_gpu_sorting_pipeline_readback.h:13` has `TestSortResultSink : public ISortResultSink` — the ONE place where an I* is genuinely used for a test seam).

**Ad-hoc replacement in production (if any):** Direct concrete types everywhere else. The interface → concrete jump is mechanical.

**Verdict:** **DELETE** all single-implementation interfaces EXCEPT `ISortResultSink` (used in tests) and `IGPUSorter` (real poly). Merge each interface's methods onto its impl; delete the `.h` file. Removes ~1,000 LOC of dead abstraction.

**Effort to resolve:** 1 week (13 interfaces × ~4h each = ~52h). Regression risk: low (mechanical); compile-time speedup non-trivial.

---

### 12. Instance pipeline `InvariantViolationReason` discrimination (partially abandoned)

**Design quality:** Excellent — ~30 fine-grained reason codes classified into 5 `InvariantViolationClass` buckets, each with a unique metric route (`instance_pipeline/invariant/atlas|cull|sort|raster|tile_runtime`).

**Location:** `modules/gaussian_splatting/renderer/instance_pipeline_contract.h` (455 LOC).

**Production consumers:** The framework IS called — contradicts the seed's zero-site claim — at:
- `resident_instance_contract_publisher.cpp:641, 643, 645, 647` (production)
- `render_streaming_orchestrator.cpp:1467, 1906, 1910, 1913, 1916, 1985, 2000` (production)
- `tile_renderer.cpp:354, 363` (production)

**BUT**: only ONE specific reason enum value is ever branched on for behavior — `InvariantViolationReason::CULL_INSTANCE_COUNT_ZERO` at `render_streaming_orchestrator.cpp:2006`. The other ~29 reason values only drive log text via `get_violation_reason_name`/`get_violation_route`. The `violation_class` bucket is used for metric labeling, not for behavior.

**Test consumers:** Limited.

**Ad-hoc replacement in production (if any):** N/A — the contract IS what captures the failure. But the failure handling is uniform regardless of reason (skip the frame, log once per reason).

**Verdict:** **KEEP-DORMANT.** The fine-grained reason enum is legitimate diagnostic infrastructure. The seed finding underrated this — if a user files "pipeline not ready, reason=SORT_CHUNK_DISPATCH_BUFFER_MISSING" as a bug, the maintainer saves hours. **However**: the `evaluate_streaming_activation` function at `:384-409` is a tiny duplicate — its logic is re-implemented inline at `render_streaming_orchestrator.cpp:1905-1987`. Collapse to one path.

**Effort to resolve:** 2 hours (consolidate the two parallel invariant-evaluation call sites).

---

### 13. `DeferredDeletionQueue` (RE-WIRED since seed drafted)

**Status update:** The seed stated the queue is instantiated nowhere. **Current state:** wired in `render_types/render_facade_state_types.h:50`, processed each frame at `gaussian_splat_renderer.cpp:2088`, drained at shutdown at `:1177`, and has one enqueue site at `render_sorting_orchestrator.cpp:205`.

**Remaining gap:** `GPUBufferManager::_destroy_buffer_set` and `BatchedAsyncReadback` still raw-`device->free(...)` without queuing. Partial migration.

**Verdict:** Not fully abandoned — **but the migration is incomplete.** Spawn a task to migrate the remaining raw-free sites.

---

### 14. `GPUBuffer` RAII wrapper

**Design quality:** Textbook move-only RAII handle around `(device, rid)`.

**Location:** `modules/gaussian_splatting/renderer/gpu_buffer_raii.h` (58 LOC).

**Production consumers:** 1 file, 2 instantiations — `render_sorting_orchestrator.cpp:460-461` wraps `keys_buffer` and `values_buffer` for scope-based cleanup on a bail-out path. `gaussian_splat_renderer.cpp:64` `#include`s the header but never uses `GPUBuffer`.

**Test consumers:** 0.

**Ad-hoc replacement in production (if any):** The rest of the ~3,500-LOC renderer slice manages raw `RID` + `RenderingDevice*` pairs with manual `device->free()` calls — the exact pattern the RAII wrapper was designed to eliminate.

**Verdict:** **ENABLE.** Migrate remaining raw-owned `RID`s to `GPUBuffer` where scope-based release is valid (not the long-lived orchestrator-owned buffers). Risk: low. Leverage: moderate (prevents future RID leak issues like #257).

**Effort to resolve:** 1 week for a careful migration; 2 days for the biggest offenders.

---

### 15. Shader validator SCsub hook

**Design quality:** Fine — `scons gs_validate_shaders=yes` triggers a build-time validation command.

**Location:** `modules/gaussian_splatting/SCsub:119-120, 239-246`.

**Production consumers:** None in CI. `.github/workflows/gaussian_shader_validation.yml:88` bypasses the SCsub hook and calls `compile_shaders.py` directly. The hook is designed for developer-machine invocation, but CI is the only place where validator failures would be enforced.

**Verdict:** **DELETE** the SCsub hook (since CI doesn't use it) OR **WIRE CI to use it** (pass `gs_validate_shaders=yes` to the build step, drop the direct python call). The second option is 5 LOC in the workflow.

**Effort to resolve:** 1 hour (either direction).

---

### 16. Persistence serializer (GaussianSceneSerializer + GaussianIncrementalSaver)

**Design quality:** Complete binary format, chunk-based, metadata + animation + forward/backward-compat version handling.

**Location:**
- `modules/gaussian_splatting/persistence/gaussian_scene_serializer.cpp` (1,060 LOC)
- `modules/gaussian_splatting/persistence/incremental_saver.cpp` (1,111 LOC)
- Plus headers (~390 LOC)

**Size:** ~2,560 LOC total.

**Production consumers:** Internal only — `incremental_saver.cpp:230, :995` instantiate their own `GaussianSceneSerializer`. No production code in `renderer/`, `core/`, `nodes/`, `editor/`, or `io/` calls either class. GDScript binding at `gaussian_scene_serializer.cpp:119-125` exposes the API to scripts.

**Test consumers:** 19+ instantiations in `tests/test_persistence_roundtrip.h`.

**Ad-hoc replacement in production (if any):** None — Gaussian scenes are loaded from `.ply`/`.ckpt`/`.spz`/`.gsplatworld` formats via `io/` loaders. The custom scene-serializer binary format is not used by any internal flow. It is effectively a GDScript-only feature.

**Verdict:** **KEEP-DORMANT** pending decision: is this a supported GDScript API? If yes, keep and tag as public API. If no, delete (~2,560 LOC). Likely answer: the persistence format was built to back `GaussianAnimationStateMachine` save/load; since animation is dormant (#2), persistence is effectively dormant too.

**Effort to resolve:** 1 hour to audit GDScript usage; 1-2 days to delete if unused.

---

### 17. `AssetDependencyManager` singleton

**Design quality:** Asset ID registry with reverse path lookup, dependency tracking, singleton pattern.

**Location:**
- `modules/gaussian_splatting/asset_management/asset_dependency_manager.cpp` (763 LOC)
- `modules/gaussian_splatting/asset_management/asset_dependency_manager.h` (190 LOC)

**Production consumers:** Zero. `AssetDependencyManager::singleton` at `asset_dependency_manager.cpp:11` is never assigned (no `singleton = memnew(…)`). `AssetDependencyManager::get_singleton` is never called from production. The class is `GDREGISTER_CLASS`'d at `register_types.cpp:143` — GDScript-only API.

**Test consumers:** 4 (`tests/test_asset_dependency_manager.cpp:68, 90, 120, 164`).

**Ad-hoc replacement in production (if any):** Asset IDs flow through `GaussianStreamingSystem::asset_id` fields (uint32_t) directly; no registry mediates the identity.

**Verdict:** **KEEP-DORMANT** pending public-API audit. The singleton pattern is suspicious (singleton never assigned = misdesign); if kept, finish the pattern by wiring `memnew` at module init.

**Effort to resolve:** 1 hour audit, 2 hours delete-or-finish.

---

## The pattern

Three cultural/organizational root causes jump out of the code.

**1. "Land the scaffolding, land the feature later" habit.** Every dormant subsystem here — `ClusterCuller`, `GaussianAnimationStateMachine`, `ChunkLODMetadata`, painterly pass-graph introspection, `enable_two_stage_sort` — was landed *complete as infrastructure* with the hook for integration left for "next PR." The next PR never came. The module has shipped several feature slices (streaming, LOD, painterly) that have this shape: a pristine subsystem shelf plus an ad-hoc path around it in the production code. This is the most expensive form of technical debt because the work was done *correctly* — it's just disconnected.

**2. Single-impl interfaces as a governance ritual.** The 13 single-implementation `I*` interfaces don't reflect actual abstraction need — they reflect a policy ("programs to interfaces"). When the policy is applied uniformly instead of judiciously, every new concrete class gets a matching interface no one will ever override. The result: ~1,000 LOC of vtable indirection that buys nothing. The honest signal is that in 4-6 weeks of architecture work, only `IGPUSorter` (3 impls) and `ISortResultSink` (test fake) were genuinely used for polymorphism.

**3. "Enum-capture then stringify" anti-pattern.** The module repeatedly builds a rich typed decision (fallback route, invariant-violation reason, pressure source/reason) and then collapses the decision to a format string for a log. This yields zero behavior change between enum values — all discrimination is thrown away at the boundary. If a decision doesn't drive different behavior, the enum should not exist. If it should drive different behavior, the caller is buggy.

Underneath the pattern is a quieter habit: **measurement without observation**. Each of these subsystems produces excellent telemetry. No one has read the telemetry in weeks. If the off-by-default diagnostics (#10) and the discarded reason codes (#12) were *actually consulted* during debugging, the unwired sites would have been caught the same day they shipped.

## Top 5 highest-leverage enablements

1. **DeferredDeletionQueue migration (complete partial wiring)** — finish migrating `GPUBufferManager::_destroy_buffer_set` and `BatchedAsyncReadback` to the queue. Root cause of issues like #257 class. ~1 day. Highest ROI on this list.
2. **Enable compute fallback Routes (#6)** — make `RETRY_NEXT_FRAME` and `DISABLE_STAGE` do what their names promise. Unlocks graceful degradation on shader-validation failures. ~2 days.
3. **Enable dev-build diagnostics (#10)** — 3 lines, dramatic drop in "please enable this flag and repro" round-trips with users. ~4 hours.
4. **ClusterCuller wire-up (#1)** — if a coarse-cull benchmark shows >5% gain, finish the integration. 2 days. But only if the benchmark is first.
5. **GPUBuffer RAII migration (#14)** — prevent the next RID-leak class bug. 1 week.

## Top 5 safest deletions

1. **Single-implementation `I*` interfaces (#11)** — ~1,000 LOC, 13 files, mechanical refactor. Except `ISortResultSink` and `IGPUSorter`.
2. **`ChunkLODMetadata` + `LODDebugStats` (#3)** — 150 LOC, zero production consumers.
3. **Painterly pass-graph introspection (#4)** — `passes` vector + `PassNode` + `get_passes`/`find_pass`. 150 LOC. Keep TextureSlot + TextureInfo.
4. **`PipelineFeatureSet::enable_two_stage_sort` (#9)** — 30 LOC across 5 files. One retired setting.
5. **`StageValidationHarness::all_valid` + `summarize_failures` (#7)** — 30 LOC. Collapse harness to a free-function `validate_stage()`.

## Blind spots

- **Shader-side abandonments.** I audited C++. GLSL shaders under `modules/gaussian_splatting/shaders/includes/*.glsl` and `compute/*.glsl` may contain equivalent abandonment (unused uniforms, dead `#ifdef FEATURE_X` paths). The `cluster_cull.glsl` compilation hint is the tip.
- **GDScript-exposed API.** `AssetDependencyManager`, `GaussianAnimationStateMachine`, `GaussianSceneSerializer`, `GaussianIncrementalSaver`, `ClusterCuller` are all `GDREGISTER_CLASS`'d. I classified them as abandoned based on C++ callers, but if any external GDScript project depends on them they're effectively public API.
- **Editor plugins / node subsystem.** I did not audit `modules/gaussian_splatting/editor/`, `modules/gaussian_splatting/nodes/`, or `modules/gaussian_splatting/io/` (beyond loader references) for abandoned infrastructure. These likely harbor more.
- **Tool output drift.** A few seed findings were outdated by 1-2 weeks of repair work (DeferredDeletionQueue wired, streaming validators called). The module is moving fast enough that a snapshot-style audit should be refreshed every ~2 weeks, not at quarter-boundary.
- **Dead `#include`s.** `gaussian_splat_renderer.cpp:64` `#include "gpu_buffer_raii.h"` but never uses `GPUBuffer`. I spot-checked one; a full scan is its own audit.
- **The `renderer_interfaces.h` 266-LOC header** — I classified it as abandonment, but it may be *doctrinal* abandonment: the team has a philosophy ("every renderer method is explicit surface"). That's a legitimate rationale even without polymorphism. The verdict on #11 should be confirmed with the team before mass-delete.

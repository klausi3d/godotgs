# Streaming Hub (gaussian_streaming.cpp) — Deep Audit

## Summary

Overall grade: **D+**. `gaussian_streaming.cpp` is a 4,928-line god-file that already shows signs of an incomplete extraction pass (ISSUE-006): eight `streaming_*` subsystem files exist and are composed in via friend classes, but the hub still owns primary-chunk building, layout-hint validation state, residency-request state transitions, the frame pipeline orchestrator, a 400-line `end_frame` analytics dump, a 400-line `_build_streaming_diagnostics_snapshot`, and a sync-fallback queue. The design is sound in the small — clear invariant checks, named admission-gate enums, structured diagnostics — but the hub class has ~18 responsibilities and a ~500-line `end_frame` / analytics path that is effectively untestable in isolation. More serious: there are **two correctness hazards** (global unlocked `HashMap` keyed by raw `this` pointers, and a missing `splat_skip_factor==0` guard that infinite-loops) and **one resource-leak hazard** (ProjectSettings `settings_changed` signal never disconnected). The file should be split along the seams already hinted at by the existing friend-class decomposition.

## What this code does

`GaussianStreamingSystem` (RefCounted, `GDCLASS`) is the orchestrator for per-frame Gaussian-splat chunk streaming. Per frame it: resolves a camera transform → updates visibility via `StreamingVisibilityController` → evicts for VRAM budget via `StreamingEvictionController` → admission-gates chunk loads via `ResidencyBudgetController` → enqueues chunks either to the async `StreamingUploadPipeline` (pack thread + upload ring) or into a sync-fallback queue maintained on `SchedulerState` → processes the upload queue → builds the per-frame visible chunk list → runs multi-horizon predictive prefetch → publishes to the global atlas GPU buffers via `StreamingGlobalAtlasRegistry`. It also owns: primary-chunk layout (contiguous or Morton-sorted or layout-hint-driven), per-chunk quantization config, LOD blend factors, generation-tracked multi-asset registration with a dense-id remap for instance GPU data, residency-request begin/collect/finalize protocol, and a very large analytics/diagnostics snapshotter.

## File anatomy

| Line range | Size | Region | Notes |
|---|---:|---|---|
| 1–24 | 24 | Includes | 10 project headers, `<cfloat>`, `<cstdint>`, `<algorithm>`, `<utility>`. |
| 26–674 | 648 | Anonymous namespace (local helpers) | Constants, layout-hint validation machinery (21 reason codes, 5 categories), Morton-10 encoder, transform/projection NaN guards, queue-pressure wrappers. Holds the global `g_layout_hint_failure_states` HashMap (148). |
| 676–679 | 4 | Section banner | "PackTelemetry implementation" — but nothing by that name lives here. Stale comment. |
| 680–729 | 50 | Ctor / dtor / `_release_persistent_buffer` | Destructor frees primary chunks only; does NOT disconnect ProjectSettings signal. |
| 731–752 | 22 | `_release_persistent_buffer` | Defensive size-mismatch warning. |
| 754–792 | 39 | `_bind_methods` | Only 19 of ~100 public methods are bound — the rest are engine-internal. |
| 794–948 | 155 | `initialize(Ref<GaussianData>)` | Ultra-long init: resets ~40 scheduler fields inline (duplicated in `initialize_empty`). |
| 949–955 | 7 | `initialize_with_device` | Thin wrapper that mutates `primary_device_override` around `initialize`. |
| 957–1079 | 123 | `initialize_empty` | Copy-paste of the 155-line init with `source_data.unref()` substituted. |
| 1081–1147 | 67 | `update_primary_asset_data` | Tear-down + rebuild; replicates eviction bookkeeping. |
| 1149–1222 | 74 | Config / attach / layout setters | `attach_memory_stream`, `set_config_overrides`, `clear_config_overrides`, `set_io_chunk_layout_hints`, `set_primary_chunk_layout` (field-by-field diff check). |
| 1224–1462 | 239 | Residency-request API | `begin/request_chunk/request_asset/finalize_residency_requests`, `_is_requested_chunk_in_current_generation`, `_update_requested_chunk_state*`, `_residency_request_{state,error}_name`, `_is_terminal_residency_request_error`, `_build_residency_request_status` (73-line Dictionary builder). |
| 1464–1644 | 181 | Multi-asset registry | `register_asset` (107 lines), `unregister_asset`, `set_chunk_payload_source`, `detach_source_data`. |
| 1646–1721 | 76 | DC-compat + dense-id remap | `_refresh_quantization_dc_compatibility`, `get_dense_asset_id`, `remap_instance_asset_ids` (pipeline-contract plumbing). |
| 1723–2017 | 295 | Chunk construction | `_create_chunks`, `_build_primary_chunks_from_layout_hints` (remap validation + fill), `_build_chunks_for_data` (Morton-sort path + per-chunk AABB/radius + quantization compute). |
| 2019–2170 | 152 | `_build_chunks_from_layout_hints`, `_resolve_primary_chunk_source_index` | IO-hint path. 7-arg validator call; oversized-hint splitting recomputes bounds. |
| 2172–2336 | 165 | Primary layout metrics, asset registry, dense-id pool | `_refresh_primary_chunk_layout_metrics`, `_register_primary_asset`, `_advance_asset_generation`, `_alloc_dense_id`, `_release_dense_id`, `_get_dense_generation`, `_get_asset_state`, `_get_asset_chunks`, `_make_chunk_key`, `set_chunk_radius_multiplier`. |
| 2338–2398 | 61 | `update_streaming` | Entry point — NaN input guard, runtime-ready guard, delegates to `_run_streaming_frame_pipeline`. |
| 2400–2494 | 95 | `_run_streaming_frame_pipeline` | Phase orchestrator with inline `sample_cpu_ms` lambda. |
| 2496–2603 | 108 | Telemetry / config reload / VRAM helpers | `_log_streaming_telemetry`, `_reload_config_if_dirty`, `_resolve_frame_delta_seconds`, `_compute_runtime_chunk_capacity_limit`, `_get_auxiliary_vram_overhead_bytes`, `_get_total_vram_usage_bytes`. |
| 2605–2651 | 47 | Per-frame reset / visibility glue | `_reset_per_frame_counters` (34-field bulk reset), `_update_camera_tracking`, `_handle_zero_visible_chunk_recovery`. |
| 2652–2688 | 37 | `_evict_for_vram_budget` | Inline VRAM-budget eviction loop. |
| 2690–2870 | 181 | `_load_visible_chunks` | Visible-chunk admission pipeline with queue-pressure scan throttle, cursor starvation-avoidance, eviction fallback. |
| 2872–2990 | 119 | `_build_visible_chunk_list` + `_handle_predictive_prefetch` | Multi-horizon (0.5×/1×/2×) prefetch with backlog-aware budget. |
| 2992–3034 | 43 | VRAM regulator + frame-stats logging + visibility glue | |
| 3036–3501 | 466 | **Chunk lifecycle core** | `_load_chunk` (sync path), `_assert_chunk_state_invariant` (tri-state lifecycle check), `_begin_chunk_upload`, `_rollback_pending_chunk`, `_load_chunk(asset,chunk)`, `_resolve_submission_device`, `_pack_chunk_data`, `_log_chunk_load_metrics`, `_upload_chunk_to_gpu`, `_finalize_chunk_load`, `_complete_chunk_load_common`, `_unload_chunk`. Eviction trampolines. |
| 3515–3523 | 9 | `begin_frame` | Advances ring index. |
| 3525–3775 | 251 | **`end_frame` — analytics megafunction** | ~130 `analytics_snapshot[...]=` writes. Re-summarizes queue pressure and re-latches. |
| 3777–4174 | 398 | **`_build_streaming_diagnostics_snapshot`** | Per-frame diagnostic classifier + fingerprint string (53 format fields) + category FSM + ~70-entry Dictionary builder. |
| 4176–4330 | 155 | Getters (VRAM/visible/runtime-ready/etc.) | Includes `get_visible_gaussians`, `get_visible_indices` (buffer-space), `is_runtime_ready`. |
| 4331–4365 | 35 | Config-reload trampolines + pack-thread start/stop | |
| 4366–4511 | 146 | Queue-path switching + sync-fallback queue | `_enqueue_chunk_load_request`, `_should_force_sync_fallback_for_async_stall`, `_can_use_async_pack_path`, `_get_sync_fallback_queue_depth`, `_compact_sync_fallback_queue`, `_enqueue_sync_fallback_chunk_load`. |
| 4513–4721 | 209 | `_drain_sync_fallback_chunk_loads` | Single 208-line method. Includes inline primary-relevance lambda, admission gate, re-enqueue policy, staleness flush. |
| 4723–4787 | 65 | Queue/upload glue + `_apply_config_overrides` + `_get_lod_config` | |
| 4789–4909 | 121 | Public debug dictionaries + VRAM/visible accessors | `get_chunk_culling_stats` (54-entry Dict), `get_vram_debug_stats`, `get_effective_max_chunks`, visible/effective ratios. |
| 4873–4892 | 20 | `BudgetState` methods | Belong to a different class; live here anyway. |
| 4911–4928 | 18 | `map_buffer_index_to_source` | O(chunks) linear scan per call. |

## Strengths

1. Explicit chunk-state invariant checker (`_assert_chunk_state_invariant`, 3040–3113) classifies violations into `invariant_slot_ownership_violations` / `invariant_upload_lifecycle_violations` and keeps counters — rare in this kind of code.
2. Runtime-not-loadable guards compare against a **cached structural fingerprint** so we log once per state transition, not every frame (2369–2394).
3. Visible-load scan cursor has a deliberate "restart-from-nearest when throttled" policy (2775, commented) — fairness vs. nearest-first trade-off is thought through.
4. `_complete_chunk_load_common` centralises load-bookkeeping + request-state update, used by both sync and async finalisers (3443).
5. Residency requests carry a `request_generation` stamp that lets stale responses be ignored cleanly (1325).
6. NaN/Inf input guards on `Transform3D` + `Projection` before any math (621–646, 2348).

## Top issues

1. **[severity: corruption]** `modules/gaussian_splatting/core/gaussian_streaming.cpp:148` — Global `HashMap<uint64_t, LayoutHintFailureState> g_layout_hint_failure_states` keyed by `(uint64_t)(uintptr_t)this`. No mutex. Two `GaussianStreamingSystem` instances created/destroyed on different threads (e.g. editor import + runtime) race on `insert`/`erase`. Also: address reuse after destruction leaks state from a prior instance into a new one at the same address, silently corrupting layout-hint analytics. **Fix:** move the state struct into the class as a member (it's small and purely per-instance — there is zero reason for it to be global). Kill the HashMap entirely.
2. **[severity: crash]** `modules/gaussian_splatting/core/gaussian_streaming.cpp:4250,4260` — `get_visible_indices()` reads `int skip_factor = chunk.splat_skip_factor;` with no `MAX(1u, ...)` guard, then does `for (uint32_t i = 0; i < chunk.count; i += skip_factor)`. The sister method `get_visible_gaussians` at 4218 has the guard. If `splat_skip_factor` is ever 0 (LOD code path clears it, a transient state is plausible) this becomes an infinite loop that OOMs on `indices.push_back`. **Fix:** `uint32_t skip_factor = MAX(1u, (uint32_t)chunk.splat_skip_factor);` — same pattern as 4218.
3. **[severity: crash]** `modules/gaussian_splatting/core/gaussian_streaming.cpp:4257` — Same function: `uint32_t buffer_base = chunk.buffer_slot * CHUNK_SIZE;` with no check that `buffer_slot != UINT32_MAX`. Callers are supposed to only push loaded chunks into `frame.visible_chunks` (2891 filters on `is_loaded`), but the invariant is not enforced here. `UINT32_MAX * CHUNK_SIZE` wraps to a small positive number; downstream indices silently point into the wrong buffer region. **Fix:** early-continue on `chunk.buffer_slot == UINT32_MAX` (mirror 4916).
4. **[severity: corruption]** `modules/gaussian_splatting/core/gaussian_streaming.cpp:697–701,708` — Destructor never disconnects the ProjectSettings `settings_changed` signal registered in `_connect_project_settings`. Callable-with-ObjectID means invocation is a no-op after destruction, but the dead connection is **never pruned** from ProjectSettings' connection list. Over a long editor session (repeated project loads, test runs, import flows), the connection list grows unboundedly. Also: `_connect_project_settings` is called from three places (ctor, `initialize`, `initialize_empty`) guarded only by `project_settings_connected`, so the check works — but the symmetric disconnect is missing. **Fix:** explicit disconnect in the destructor.
5. **[severity: crash]** `modules/gaussian_splatting/core/gaussian_streaming.cpp:713–724` — Destructor resolves `rd` via `primary_device_override → last_upload_device → manager->get_primary_rendering_device()`. If the `RenderingDevice` has already been torn down (module shutdown ordering during editor exit), `manager->get_primary_rendering_device()` can return a dangling pointer or the "correct" device that's mid-finalisation. The single `WARN_PRINT` path at 745 handles the null case but not the stale-but-nonnull case. **Fix:** require `GaussianSplatManager` to track a "devices finalized" flag and return nullptr thereafter; or hoist `chunk.gpu_buffer` cleanup into explicit teardown triggered before the rendering device goes away.
6. **[severity: maint]** `modules/gaussian_splatting/core/gaussian_streaming.cpp:794–948 vs 957–1079` — `initialize()` and `initialize_empty()` are near-verbatim copies (~130 lines each of state reset). A third copy of the eviction-accounting block lives at 1100–1109 / 1534–1545 / 1591–1602 (tear-down of loaded chunks: decrement count, subtract bytes, add to `evicted_bytes_total`, clear flags). Any invariant addition has to be mirrored 3–6 times. **Fix:** extract `_reset_runtime_fields()` + `_reset_scheduler_stats()` + `_accumulate_eviction_bookkeeping(StreamingChunk&)`; both init paths call the common reset + their divergent branches.
7. **[severity: maint]** `modules/gaussian_splatting/core/gaussian_streaming.cpp:3525–3775` — `end_frame()` is a 250-line analytics dump with ~130 `analytics_snapshot[...]=` writes. It also re-summarises queue pressure and mutates `upload_pipeline.queue_pressure_{active,source,reason}` state (3692–3696) — meaning analytics has write side-effects on the pipeline state machine. This makes analytics non-idempotent and makes `_build_streaming_diagnostics_snapshot` (which calls the same summariser again at 3817) a second write site 400 lines later. Hard to reason about, hard to test. **Fix:** the summarise+latch step belongs in a single call at the end of `_run_streaming_frame_pipeline`, not inside two separate Dictionary builders.
8. **[severity: perf]** `modules/gaussian_splatting/core/gaussian_streaming.cpp:4911–4927` — `map_buffer_index_to_source()` linearly scans *all* chunks every call; callers are typically in the editor-pick / selection hot path. For a 64k-chunk asset every pick ray runs an O(N) scan. **Fix:** maintain a `HashMap<uint32_t buffer_slot, uint32_t chunk_idx>` alongside `atlas_allocator`, updated at `_finalize_chunk_load` / `_unload_chunk`. Reverse-lookup is then O(1).
9. **[severity: perf]** `modules/gaussian_splatting/core/gaussian_streaming.cpp:1884–1934` — `_build_chunks_for_data` disables Morton sort above 2M splats (`kMortonSortSplatThreshold`) and falls back to contiguous chunk partitioning, emitting a `WARN_PRINT`. For any dataset that pushes streaming at all (tens of millions of splats), the Morton path *never* runs — so "spatial chunking" is effectively dead in the real use case. Worse, contiguous layout means frustum culling degrades to ~0 effectiveness for real scans. **Fix:** implement the Morton sort in tiled/external-memory fashion (sort indices, not Gaussians; use `radix_sort_morton` on the 32-bit codes; O(N) time, no 30 s stall) — or explicitly document that the streaming pipeline *requires* layout hints from the importer for large assets and hard-fail otherwise.
10. **[severity: perf]** `modules/gaussian_splatting/core/gaussian_streaming.cpp:2497,3020,3315,3335` — Four independent `static` counters inside member functions (`debug_frame`, `lod_log_counter`, `gaussians_check_count`, `diag_count`). These are **process-global**, not per-instance. Tests that create a `GaussianStreamingSystem`, exercise it, destroy it, create another — the `gaussians_check_count <= 3` gate is already exhausted. Also not thread-safe (increment on concurrent pack workers). Also: first 3 chunks of the *second* streaming system produce zero diagnostics. **Fix:** demote to instance members or atomic counters on `diagnostics`.
11. **[severity: maint]** `modules/gaussian_splatting/core/gaussian_streaming.cpp:4513–4720` — `_drain_sync_fallback_chunk_loads` is a single 208-line method with an inline 17-line lambda (`is_primary_chunk_relevant`), three `_update_requested_chunk_state` call sites in the same function with slightly different arguments, and a re-enqueue-on-deferral policy mixed with a staleness-flush policy. Cyclomatic complexity > 25. **Fix:** split into `_sync_fallback_compute_drain_budget`, `_sync_fallback_process_one`, `_sync_fallback_flush_if_stale`.
12. **[severity: maint]** `modules/gaussian_splatting/core/gaussian_streaming.cpp:676–678` — Banner comment reads "PackTelemetry implementation (DEV_ENABLED only)" but no PackTelemetry code lives here — it's in `streaming_upload_pipeline.cpp`. Stale from a previous extraction. Small thing but symptomatic of an incomplete refactor.
13. **[severity: maint]** `modules/gaussian_splatting/core/gaussian_streaming.cpp:3777–4174` — `_build_streaming_diagnostics_snapshot` is 398 lines. Builds a `fingerprint` string with 53 fields via `vformat`, then compares against `diagnostics.last_logged_fingerprint` to decide whether to emit a `WARN_PRINT`. Every frame constructs the full 53-field string even when not emitted — unnecessary allocation/formatting work in the steady state. **Fix:** split into `diagnostics_classify()` (returns a `DiagnosticsResult` struct) + `diagnostics_to_dictionary()`, and only build the fingerprint when `has_failure` is true and we're within the log cadence.
14. **[severity: maint]** `modules/gaussian_splatting/core/gaussian_streaming.cpp:721–725` — Destructor iterates `chunks` (primary only) to free `chunk.gpu_buffer` — but I can find no site that *sets* `chunk.gpu_buffer` (it's unused vestigial field inherited from a pre-atlas design). Also does not touch `AtlasAssetState::asset_chunks[*].gpu_buffer` for non-primary assets. Dead code masking a real hole. **Fix:** delete the `gpu_buffer` field from `StreamingChunk` or iterate all assets and drop the confusion.

## Cross-cutting patterns

- **Friend-class composition is already in place** (header lines 31–34: `StreamingUploadPipeline`, `StreamingEvictionController`, `StreamingVisibilityController`, `StreamingGlobalAtlasRegistry`). The subsystem .cpp files own the real state transitions; this hub mostly glues and forwards. That means most of the hub's remaining size is (a) analytics/diagnostics builders, (b) initialization fan-out, (c) chunk construction, (d) residency-request protocol. These are the seams.
- **Inline state-reset blocks** are a strong anti-pattern here (1001–1078, 794–843). Any new scheduler field forces touching 2–3 mirror sites.
- **`analytics_snapshot` is a `Dictionary` passed through both `end_frame` and `_build_streaming_diagnostics_snapshot`** — the builders share no schema, drift is already visible (e.g. both sides re-latch queue pressure state).
- **Public Dictionary-returning methods** (`get_chunk_culling_stats`, `get_lod_debug_stats`, `get_vram_debug_stats`, `get_quantization_stats`, `get_streaming_analytics`, `get_task_debug_state`, `_build_streaming_diagnostics_snapshot`) all construct dictionaries from scratch with overlapping keys — a typed `StreamingDiagnosticsSnapshot` POD + one `to_dictionary()` would eliminate ~700 lines of Dictionary-building.
- **`static` function-scope counters** appear 4 times and are all wrong (process-global instead of instance-local).

## Proposed split seams

Five concrete boundaries, each file < 1,500 lines:

| New file | Source lines | Responsibility |
|---|---|---|
| `streaming_layout_hints.cpp/.h` | 26–541, 2019–2170, 1760–1882 (+ `_create_chunks` at 1723–1758, `_build_chunks_for_data` at 1884–2017, `_refresh_primary_chunk_layout_metrics` at 2172–2223, `_resolve_primary_chunk_source_index`) | The entire layout-hint validator FSM, failure-state tracking, chunk constructors (contiguous/Morton/hints), and layout metrics. ~1,000 lines. **This also kills the global HashMap** — the failure state becomes a member of the new class, passed by reference. |
| `streaming_residency_requests.cpp/.h` | 1224–1462, 2255–2309 (dense-id pool can stay co-located) | Begin/collect/finalize residency requests, request-generation stamping, status Dictionary builder, name-of-error/state enums, terminal-error classifier. ~240 lines. |
| `streaming_frame_pipeline.cpp` | 2338–3034 (all `_run_streaming_frame_pipeline` / `_evict_for_vram_budget` / `_load_visible_chunks` / `_build_visible_chunk_list` / `_handle_predictive_prefetch` / `_update_vram_regulator` / `_log_streaming_frame_stats`) | The per-frame pipeline orchestrator. ~700 lines. |
| `streaming_chunk_lifecycle.cpp` | 3036–3513 (load/pack/upload/finalize/unload + invariant checks + sync fallback queue at 4411–4721) | The chunk lifecycle state machine. Strong internal cohesion. ~900 lines including sync fallback. |
| `streaming_diagnostics_reporter.cpp/.h` | 3525–4174 (`end_frame` analytics + `_build_streaming_diagnostics_snapshot` + `get_chunk_culling_stats`) | All Dictionary construction and fingerprint logic. ~700 lines. Expose a typed `StreamingDiagnosticsSnapshot` struct so callers don't stringly-type lookups. |

What remains in `gaussian_streaming.cpp` becomes a ~400-line orchestrator: ctor/dtor, `_bind_methods`, the two `initialize*` paths (after common-reset extraction), `register_asset`/`unregister_asset`, small accessors, `begin_frame`/`end_frame` delegation. That is a sane size for a coordinator.

## Recommended refactor moves

- **P0 (correctness — do first, in-place, no split needed, each 1–3 h):**
  - Fix issue #2 (`splat_skip_factor` guard at :4250).
  - Fix issue #3 (`buffer_slot == UINT32_MAX` guard at :4257).
  - Fix issue #1 (promote `g_layout_hint_failure_states` to a member).
  - Fix issue #4 (disconnect ProjectSettings signal in destructor).
  - Total: ~1 day.

- **P1 (structure — 2–3 days each):**
  - Extract `streaming_layout_hints` (issue #1's member becomes the class's own state; issue #9's Morton work lands here too).
  - Extract `streaming_diagnostics_reporter` + typed snapshot struct (issues #7, #13).
  - Extract init reset helpers (issue #6).

- **P2 (polish — 1–2 days each):**
  - Extract `streaming_residency_requests`.
  - Extract `streaming_frame_pipeline` + `streaming_chunk_lifecycle` (larger; worth waiting until P1 proves the pattern).
  - Fix `static` counters → instance fields (#10); fix `map_buffer_index_to_source` O(N) (#8); delete dead `chunk.gpu_buffer` field (#14); split `_drain_sync_fallback_chunk_loads` (#11).

## Blind spots

- `StreamingUploadPipeline`, `StreamingEvictionController`, `StreamingVisibilityController`, `StreamingGlobalAtlasRegistry`, `StreamingQueuePressureController`, `ResidencyBudgetController`, `VRAMBudgetRegulator`, `GaussianAtlasAllocator`, `GaussianMemoryStream`, `ChunkPayloadSource` — all referenced but defined outside this slice. Audit assumes their methods keep the contracts suggested by their callers here.
- `pack_gaussians_range`, `PackedGaussian`, `SHCompressionMetrics` — packing path in `gaussian_gpu_layout.h` / `gaussian_data.cpp`.
- `LODConfig` and `g_lod_config` — singleton read in `_get_lod_config`; concurrency guarantees not inspected.
- `GaussianSplatManager::ScopedSubmissionLock`, `acquire_submission_device`, `is_shared_submission_device` — the device-lock protocol is critical for `_load_chunk` (3185–3201) but not visible in this slice.
- `StreamingTelemetryAdapter::apply_queue_pressure_{analytics,diagnostics}` — double-applied (end_frame + diagnostics_snapshot) for different keys; the adapter's actual effects on `analytics_snapshot` schema were not traced.
- Test-only methods at header lines 288–319 (`_test_sync_global_atlas_state`, etc.) — consumers in `test_gpu_streaming.cpp` not read.
- Pipeline contracts (`renderer/instance_pipeline_contract.h`, `renderer/pipeline_io_contracts.h`) — `remap_instance_asset_ids` at 1675 consumes `InstanceDataGPU`; contract semantics (e.g. `ids[0]`, `lod[1]`) were not cross-checked against the contract header.

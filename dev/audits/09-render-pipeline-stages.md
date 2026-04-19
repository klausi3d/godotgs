# Render Pipeline Stages & Contracts — Deep Audit

Unit 09 — pass-graph design, stage-contract enforcement, invariant checking, pipeline feature sets.

Files under audit (LOC measured):

- `modules/gaussian_splatting/renderer/render_pipeline_stages.cpp` — 2,517 LOC
- `modules/gaussian_splatting/renderer/render_pipeline_stages.h` — 111 LOC
- `modules/gaussian_splatting/renderer/painterly_pass_graph.cpp` — 352 LOC
- `modules/gaussian_splatting/renderer/painterly_pass_graph.h` — 96 LOC
- `modules/gaussian_splatting/renderer/pipeline_feature_set.cpp` — 344 LOC
- `modules/gaussian_splatting/renderer/pipeline_feature_set.h` — 62 LOC
- `modules/gaussian_splatting/renderer/pipeline_io_contracts.h` — 53 LOC
- `modules/gaussian_splatting/renderer/instance_pipeline_contract.h` — 455 LOC
- `modules/gaussian_splatting/renderer/resident_instance_contract_publisher.cpp` — 660 LOC
- `modules/gaussian_splatting/renderer/resident_instance_contract_publisher.h` — 14 LOC

## Summary

**Grade: C+** (trending D once you grade the pass graph and the contract-enforcement gap on their own).

The stage code is *working* and the error-plumbing is genuinely impressive — StageIO validation, RenderRouteUID tagging, pipeline-trace events, frame-plan snapshots, cached-render signatures. But three of the four load-bearing abstractions in this slice are half-wired or not wired at all:

1. **Pass graph is vestigial.** `PainterlyPassGraph` builds a `Vector<PassNode>` describing three passes with typed input/output slots (`painterly_pass_graph.cpp:310-352`) and exposes `get_passes()` / `find_pass()` (`painterly_pass_graph.h:90-91`). Zero callers anywhere in the module. The renderer hard-codes two dispatches in `PainterlyRenderer::execute_passes` (`interfaces/painterly_renderer.cpp:419-465`). The "graph" is a texture pool with dead metadata.
2. **Stage-boundary invariants are booleanised.** In `render_pipeline_stages.cpp` only `has_cull_buffers`, `has_sort_buffers`, `has_raster_buffers` are called (3 call sites total). The richer validators — `first_atlas_violation`, `first_cull_violation`, `first_sort_violation`, `first_raster_violation`, `first_tile_runtime_violation`, `evaluate_streaming_activation`, `get_violation_route` — have **zero** call sites in this file. When a stage skips, the skip reason is a hand-rolled `"Sort skipped: instance buffers missing"` string, not a `InvariantViolationReason`. Contrast with `render_streaming_orchestrator.cpp:1905-1988` which uses the whole vocabulary.
3. **Feature set is largely cosmetic inside the pipeline.** Of 141 occurrences of `enable_*` flags module-wide, exactly **one** flag feeds the stage path (`enable_fast_raster` → `plan.compute_raster_policy`, `render_pipeline_stages.cpp:605-607`). `enable_two_stage_sort` reaches no sort stage switch in this file; `enable_packed_stage_data`, `enable_tighter_bounds`, `enable_sh_amortization` only surface as `request_*` fields mirrored into `TileRenderer::RenderParams` (`render_pipeline_stages.cpp:1833-1840`). The validation / tier / provenance / project-settings machinery in `pipeline_feature_set.cpp` is ~300 lines of ceremony for ~10 lines of consumption.

The cpp is also a 2.5k-line god-file: frame planning, frame context prep, hashing for cache signatures (~200 LOC of lighting/cull signature hashing that duplicates logic for wind + sphere-effector project settings also read elsewhere), five nested stage structs, tile fallback, painterly fallback, compositor cache pokes, performance metric EMAs, and route-UID chooser logic all live in one file.

## What this code does

`RenderPipelineStages` is the entry-point for a Gaussian-Splat frame. It is called from `GaussianSplatRenderer::render_sorted_splats` via `execute_frame_entry`. The flow is:

1. `prepare_frame_context` (`.cpp:619-697`) — resolves the viewport override format, builds `RenderFrameContext.deps` (an `IFrameStateView` / `IFrameMutationAccess` bag that's 15+ pointers), calls `renderer->update_pipeline_features(state_view.get_rendering_device())`, and `DEV_ASSERT`s the deps are complete.
2. `execute_frame_entry` (`.cpp:701-842`) — builds a `RenderFramePlan` via `build_frame_plan` (static, `.cpp:585-615`), rebinds the provider over the copied deps (the "stale frame_plan pointer" guard, `.cpp:747-754`), runs cull → sort → `render_sorted_splats_with_context`.
3. `CullStage::execute` (`.cpp:855-971`) — takes the `GPUCuller`, checks `has_cull_buffers`, binds `GPUCuller::InstancePipelineInputs`, calls `pipeline->cull_for_view` which delegates back to `renderer->cull_for_view`.
4. `SortStage::execute` (`.cpp:985-1218`) — similar, with extra domain-mismatch validation (`GAUSSIAN_GLOBAL` vs `CHUNK_REF` vs `SPLAT_REF`).
5. `RasterCompositeStage::execute` (`.cpp:1277-1430`) — dispatches `RasterStage::resolve_painterly_output`, `try_reuse_cached_render`, then either reuses cache or runs painterly-or-baseline; composes signatures for the render cache; then runs `CompositeStage::execute` (`.cpp:2164-2249`) which drives `OutputCompositor::integrate_final_output`.
6. `render_sorted_splats_with_context` (`.cpp:2260-2489`) — the top-level orchestrator for the post-cull/sort frame: emits skip-path metrics, route-UID events, calls `execute_raster_composite_pipeline`, logs stage results, increments frame counter.

Support types: `PainterlyPassGraph` holds painterly render targets (color/depth/edge/stylized) plus the dead pass table. `PipelineFeatureSet` is a config bag. `InstancePipelineContract` (header-only inline funcs) is the buffer-presence validator. `ResidentInstanceContractPublisher::publish` builds the in-memory atlas used when streaming is off.

## Contract-invocation audit

**Claim:** earlier module-wide audit said the contract system is "well-designed but never invoked at stage boundaries."

**Verified against `render_pipeline_stages.cpp`:**

| Validator | Call sites in render_pipeline_stages.cpp | Notes |
|---|---|---|
| `first_atlas_violation` | **0** | |
| `first_cull_violation` | **0** | |
| `first_sort_violation` | **0** | |
| `first_raster_violation` | **0** | |
| `first_tile_runtime_violation` | **0** | |
| `evaluate_streaming_activation` | **0** | |
| `get_violation_route` | **0** | |
| `get_violation_class` / `_name` / `reason_name` | **0** | |
| `make_violation` | **0** | |
| `has_cull_buffers` | 1 (line 887) | |
| `has_sort_buffers` | 1 (line 1028) | |
| `has_raster_buffers` | 1 (line 1591, tile fallback) | |
| `has_atlas_buffers` | 0 | |

**3 call sites total, all boolean.** The three callers only ask "are buffers there"; when the answer is no, they return a string-literal skip reason and a generic `RenderFallbackReason::DATA_UNAVAILABLE` — discarding the information the enum carries (which buffer, what invariant). The `InvariantViolationReason` never reaches the metrics, pipeline-trace event stream, or the `RenderFallbackReason` that downstream diagnostics consume.

**Elsewhere in the module** (for contrast; confirms the vocabulary works and is *close* to the stage code):

- `render_streaming_orchestrator.cpp:1467-1985` — uses all four `first_*_violation`, `make_violation`, `evaluate_streaming_activation`, `get_violation_route`, `get_violation_class_name`, `get_violation_reason_name`. Real enforcement lives here.
- `tile_renderer.cpp:354-366` — single `first_tile_runtime_violation` + route/class/reason logging. In `DEBUG`/`TESTS` builds it's `ERR_FAIL_V_MSG`; in release it's `ERR_PRINT_ONCE + return false`.
- `resident_instance_contract_publisher.cpp:258-651` — post-publish invariant sweep using all four `first_*_violation`.
- `render_instancing_orchestrator.cpp:51-53` — three `has_*_buffers` booleans.
- `render_sorting_orchestrator.cpp:169` — one `has_sort_buffers` boolean.

**Conclusion: claim is corroborated.** The stage boundaries are the place the validators would be most useful — a `CullStage::execute` that returns `InvariantViolationReason::CULL_VISIBLE_CHUNK_BUFFER_MISSING` is qualitatively different to "instance buffers missing" — and they're exactly the place that doesn't call them. Worse, the publisher (`resident_instance_contract_publisher.cpp`) validates the invariant *after* publishing, the streaming orchestrator validates it *before* activation, but the consumer in between (the stages) re-asks only the top-level boolean and invents new skip strings.

## Strengths

- **StageIO + StageIOValidationConfig** (`.cpp:509-552`) is a nice discipline: every stage fills an `input_count / output_count / input_buffer / output_buffer / validated / validation_failed / validation_error`, and the `_finalize_stage_io` helper gates `validation_failed` only when `p_io.validated` is set. Simple, read well.
- **Pipeline-trace events with RenderRouteUID** (`.cpp:173-196`, many call sites) give an actual per-frame audit trail keyed on named UIDs like `INSTANCE_CULL_GPU`, `COMMON_FAIL_NO_DEVICE`, `COMMON_SKIP_NO_DATA`. Good debuggability.
- **FrameStateProvider rebind pattern** (`.cpp:747-754`) — there is a real bug class (stale `frame_plan` pointer after copying `RenderFrameContext`) and it's explicitly guarded with a comment. Rare to see this written down.
- **`_compute_color_grading_signature` / `_compute_lighting_signature` / `_compute_cull_config_signature`** (`.cpp:260-494`) — the cached-render reuse pipeline is principled: if and only if all four signatures match *and* the `content_generation` matches, reuse the cached render. This is how you correctly invalidate a frame cache under dynamic lights.
- **Domain validation in SortStage** (`.cpp:1029-1101`) — `CHUNK_REF`/`GAUSSIAN_GLOBAL`/`SPLAT_REF` transitions are checked in both directions (chunk with no buffers → fail; global with instance buffers → fail), and the expected-output-domain check (`.cpp:1154-1163`) catches GPUCuller/GPUSorter backend desynchronization.
- **`execute_frame_entry` copies the RenderFrameContext** (`.cpp:710`) specifically so `frame_plan = &frame_plan` is safe even if the caller passed a `const RenderFrameContext &` whose `frame_plan` pointer is null. Deliberate.

## Top issues

- **[severity: maint]** `renderer/render_pipeline_stages.cpp:887,1028,1591` — the three `has_*_buffers` calls discard the first-violation reason — when a stage skips it returns a string-literal reason (`"Sort skipped: instance buffers missing"`) and a generic `RenderFallbackReason::DATA_UNAVAILABLE`. The enum-carrying reason (e.g. `SORT_VISIBLE_CHUNK_BUFFER_MISSING`) is never surfaced. **Why it matters:** when the stage skips, the diagnostics tell you "something was missing" but not *what was missing*; for a buffer-ownership / publisher-ordering bug this is the difference between "five seconds of debugging" and "an afternoon". **Fix:** at each stage entry call `first_cull_violation` / `first_sort_violation` / `first_raster_violation`, stash the `InvariantViolationReason` in a new `StageResult::invariant_reason` field, log via `get_violation_route` / `get_violation_class_name` / `get_violation_reason_name`, and map the enum to the hand-rolled skip string. Pattern exists already in `render_streaming_orchestrator.cpp:1905-2003`.

- **[severity: maint]** `renderer/painterly_pass_graph.cpp:310-352`, `painterly_pass_graph.h:20-38,90-91` — `PassNode`/`PassType`/`PassId` and the built `passes` vector are dead data; `get_passes()`/`find_pass()` have no callers in the module. `PainterlyRenderer::execute_passes` hard-codes sobel+brush dispatches (`interfaces/painterly_renderer.cpp:419-465`). **Why it matters:** the name "pass graph" is a lie. New contributors will reasonably expect "add a pass" to mean "push a `PassNode`"; it doesn't. Adding mesh-shader / VRS passes later requires replacing this, not extending it. **Fix direction:** *either* wire `execute_passes` to iterate `get_passes()` and dispatch via a `PassType`-keyed visitor (requires lifting the hard-coded sobel/brush calls into named handlers); *or* delete the enum/vector and rename the class to `PainterlyTexturePool`. The hybrid we have now is the worst of both.

- **[severity: maint]** `renderer/render_pipeline_stages.cpp:260-494` — 234 lines of signature-hashing (color-grading, lighting, cull-config) live inside an anonymous namespace in a 2.5k-LOC file. The lighting-signature code also reads `rendering/gaussian_splatting/animation/wind_*` and `rendering/gaussian_splatting/effects/sphere_effector_*` project settings *again* at `.cpp:1678-1726` inside `render_tile_fallback` with **identical fallbacks and identical `StringName` tables** — 40 lines of duplicated project-settings reads, in the same compilation unit. **Why it matters:** two sources of truth for wind/effector config. Any new lighting / animation param has to be added in both places or the render cache will incorrectly reuse frames. **Fix:** extract `GSLightingParams::load_from_project_settings()` to a shared struct, use it in both signature computation and `render_params` population.

- **[severity: maint]** `renderer/pipeline_feature_set.cpp:1-344` — the four flags `enable_two_stage_sort`, `enable_packed_stage_data`, `enable_tighter_bounds`, `enable_sh_amortization` have elaborate project-settings registration, tier-preset override, provenance-snapshot tracking, runtime-capability validation, warning emission — and inside `render_pipeline_stages.cpp` only `enable_fast_raster` drives a branch (`.cpp:605-607`), the rest are mirrored as `request_*` fields on `TileRenderer::RenderParams`. `enable_two_stage_sort` has zero consumers inside this slice. **Why it matters:** someone is going to look at this and think "two-stage sort" is a toggle that exists; it's a config that propagates into metrics and provenance but flips no actual sort behavior in these files. **Fix:** prove the flags reach a shader variant or pipeline-layout branch; if any don't, either wire or delete. Kill the provenance-snapshot duplication with `loaded_provenance_snapshot` / `effective_provenance_snapshot` if nothing consumes it (grep shows `get_effective_config_snapshot` has a few bindings callers; verify they're live).

- **[severity: maint]** `renderer/render_pipeline_stages.cpp:1536-1931` — `RasterStage::render_tile_fallback` is a 395-line method that reads 20 project settings, CLAMPs every one by hand, populates ~70 `render_params.*` fields, calls `tile_renderer` via `rasterizer->render_direct`, updates tracking / metrics / overlay / overflow-autotune. This is the largest single method in the file and does five things. **Why it matters:** this is where a bug in lighting / wind / effector propagation is going to land, and `_compute_lighting_signature` (which must produce a bit-identical fingerprint for cache reuse) has to stay in sync with this inline-assignment block. Drift here = stale cached frames. **Fix direction:** extract `RenderParamsBuilder::build(render_config, project_settings, instance_buffers, frame_plan)` returning a `TileRenderer::RenderParams`, reuse from the signature computation.

- **[severity: perf]** `renderer/render_pipeline_stages.cpp:396-457` — `_compute_lighting_signature` walks every `PagedArray<RID>` visible light every frame, does 15+ `light_storage->light_get_*` RPC-style calls per light, and XORs `LIGHT_PARAM_MAX` float params each. This runs unconditionally on every sorted splat frame just to decide whether to reuse a cached painterly render. **Why it matters:** at 200 lights, we're doing ~5000 calls to `LightStorage` per frame purely for cache invalidation. The `light_get_version` is there *because* `light_set_param` doesn't always bump it (explicit comment at `.cpp:422-423`), so we can't just trust version. **Fix direction:** hash the light list once per frame in a single pass, store the fingerprint on `FrameState`; have other consumers read the fingerprint. Alternatively have `LightStorage` expose an "any inspector-mutable param changed" version.

- **[severity: maint]** `renderer/render_pipeline_stages.cpp:2260-2489` — `render_sorted_splats_with_context` is 230 lines with seven separate early-exit paths (no metrics, sort failed, zero visible, device unavailable, raster failed, cache reused, normal). Each path independently fills raster_io / composite_io / raster_result / composite_result / emits pipeline events / calls `reset_render_state_for_frame` / calls `finalize_frame`. **Why it matters:** the skip paths have drifted. Line 2353-2394 zero-visible-splats path fills `raster_result` / `composite_result` with `RenderFallbackReason::NO_VISIBLE_SPLATS`, but line 2319-2352 sort-failed path uses `p_context.metrics->sort_result.fallback_reason` (which may be `NONE`). A downstream diagnostic treating `fallback_reason != NONE` as "had a reason" will disagree with the reason captured in the prior block. **Fix:** extract a `SkipContext { StageResult raster_skip; StageResult composite_skip; const char *skip_reason_uid; }` and one `emit_skip_and_finalize(ctx, skip_ctx)` helper.

- **[severity: corruption]** `renderer/render_pipeline_stages.cpp:2221-2226` — after the cached-render block there is a dead-code comment `// FIX: NEVER skip the composite just because the render target hasn't changed. / // Godot clears the viewport each frame, so we MUST re-composite the cached render. / // Fall through to actually composite the cached render to the viewport` and the `if (p_input.raster_output.reused_cached_render) { }` block is now an empty-body conditional. **Why it matters:** this read as "intentionally no-op" but the `if` is structurally load-bearing for nothing; someone will re-introduce the bug by "cleaning up" the empty branch without reading the comment, or worse, *not* notice the comment is describing a subtle constraint (Godot clears the RT each frame). **Fix:** replace with an explicit `(void)p_input.raster_output.reused_cached_render;` and a named constant, or delete the empty branch and move the comment to the function header where it documents the invariant.

- **[severity: maint]** `renderer/render_pipeline_stages.cpp:605-607` — `plan.compute_raster_policy = (p_pipeline_features && p_pipeline_features->enable_fast_raster) ? ForceOn : Default;` is the only effect of `enable_fast_raster` in this TU. The flag name promises a "fast raster"; the actual effect is flipping the `ComputeRasterPolicy` enum from `Default` (may-fallback-to-fragment) to `ForceOn`. **Why it matters:** the flag name hides the semantics — if fragment is faster on a given device, `enable_fast_raster` makes things slower. The `PipelineFeatureSet::get_effective` already disables the flag when `p_compute_raster_enabled` is false, but "compute-raster available" ≠ "compute raster is fast". **Fix:** rename to `force_compute_raster` or document the semantics in the enum name.

- **[severity: maint]** `renderer/painterly_pass_graph.cpp:146-147` — fallback format downgrade is silent: `if (rd && !rd->texture_is_format_supported_for_usage(requested_format, p_usage)) { requested_format = RD::DATA_FORMAT_R8G8B8A8_UNORM; }`. **Why it matters:** if the viewport is HDR (say `R16G16B16A16_SFLOAT`) and storage-with-sampling isn't supported for it, we silently drop to 8-bit UNORM and clip highlights in the painterly pass. No log, no warning, no provenance. **Fix:** at minimum `WARN_PRINT_ONCE` with both formats; ideally propagate a `PainterlyRenderer` capability flag so the caller can decide to skip painterly rather than banding.

- **[severity: perf]** `renderer/resident_instance_contract_publisher.cpp:304-323` — the 2 GB staging-limit early-out runs *after* `director->build_instance_buffer_for_renderer` has materialised the full instance list, and *before* any per-chunk packing, but the descriptor build above it (`_append_chunk_descriptors_for_asset`, `.cpp:109-150`) is still run once we fall through. For the reject case we've already done the instance pass (potentially a few hundred thousand entries) and the gen-hashing (`.cpp:229-245`). **Why it matters:** not a crash — just wasted cycles on the reject path, which is taken when the user loads a huge dataset and streaming is preferred. **Fix:** compute `total_gaussians` from `assets[i].data->get_count()` before building instances; short-circuit earlier.

- **[severity: maint]** `renderer/pipeline_io_contracts.h:1-53` — `IndirectDispatchLayout` and `ClusterCullIndirectDispatchLayout` are structurally identical (6 × `uint32_t`) but have different field names (`element_count/overflow_flag/unclamped_total` vs `visible_cluster_count/visible_splat_count/clusters_culled`). Static asserts check sizeof/offsetof but nothing checks that shader-side struct matches. **Why it matters:** low — the comment-documented offsets are explicit and the struct has a `static_assert` on every field offset — but there is no cross-check that GLSL `cluster_cull_indirect` binding layout matches. A shader rename drifts silently. **Fix:** add a `// Must match cluster_cull_indirect.glsl:xxx` cross-reference comment with a line number, and a CI check (future work).

## Cross-cutting patterns

- **Seams that aren't enforced.** `RenderFrameContext::deps.validate()` (called via `DEV_ASSERT` at `.cpp:696` and `ERR_FAIL_COND` at `.cpp:706`) is the only boundary check; after the first ERR_FAIL, every nested stage re-resolves `state_view` / `state_mut` / `output_compositor` with fallback providers and defensive null checks. The defensive checks are correct but they pile up: in a 2.5k-LOC file there are 12+ `if (!output_compositor)` / `if (!gpu_culler)` / `if (!sorting_pipeline)` sites, each with its own failure path. A less-defensive style with a `StageContext` struct that carries non-null refs after `execute_frame_entry` sanity checks would eliminate them.

- **Dual-path skip handling.** Every stage has a "skip because no data yet" branch AND a "fail because something is wrong" branch. The distinction is consistently implemented (skip → `is_error=false`, fail → `is_error=true`), but the route-UID and fallback-reason assignment is duplicated: `COMMON_SKIP_NO_DATA` vs `COMMON_FAIL_SORT_FAILED` vs `COMMON_FAIL_NO_DEVICE` vs `COMMON_FAIL_NO_OUTPUT` get chosen at every stage. Pull the `(status, reason, route_uid)` mapping into a table.

- **Provider-indirection ambivalence.** Many sites do `p_input.state_view ? *p_input.state_view : fallback_provider` (`.cpp:860,992,1943,2016,2120,2169,2263`). The *intent* is "allow the caller to inject a provider; else build one from `renderer`". But the fallback provider always works because it's just wrapping `renderer`. So the indirection buys testability that no tests in-file exercise. Either the callers all pass providers (→ delete the fallback) or they all don't (→ delete the parameter).

- **StageIO shape is tight, invariant-reason shape is absent.** `StageResult` has `StageStatus status, bool is_error, String reason, RenderFallbackReason fallback_reason`. What it's missing is `InvariantViolationReason invariant_reason = InvariantViolationReason::NONE`. Adding this one field and filling it at three sites (`CullStage` line 885, `SortStage` line 1027, `RasterStage` line 1590) closes the loop on the contract-invocation gap.

## Recommended refactor moves

**P0 (must):**

- **P0.1** — *Wire stage-boundary contract validators.* Effort: ~1d. Add `InvariantViolationReason StageResult::invariant_reason = NONE`. At the 3 `has_*_buffers` call sites, replace the boolean test with:
  ```cpp
  const auto reason = GaussianSplatting::InstancePipelineContract::first_cull_violation(buffers);
  const bool cull_buffers_ok = (reason == InvariantViolationReason::NONE);
  ```
  On skip, thread `reason` into `StageResult.invariant_reason` and use `get_violation_reason_name` / `get_violation_route` in the log message + pipeline-trace event (pattern already in `render_streaming_orchestrator.cpp:2000-2003`). Small, contained, turns "stage skipped" diagnostics from `DATA_UNAVAILABLE` into the precise missing buffer.

- **P0.2** — *Decide the pass-graph's fate.* Effort: 0.5d–2d. Either (a) delete `PassNode`/`PassType`/`PassId` + `get_passes`/`find_pass` and rename to `PainterlyTexturePool` (0.5d), or (b) wire `PainterlyRenderer::execute_passes` to drive a `PassType`-keyed dispatcher using `get_passes()` (2d, requires lifting sobel/brush to a visitor). Living with the current half-wire is a long-term tax on every new painterly contributor.

**P1 (should):**

- **P1.1** — *Extract `render_tile_fallback` helpers.* Effort: ~1d. Lift the 40-line ProjectSettings wind/effector read (`.cpp:1678-1726`) out to `gs_project_settings.h` — it's already duplicated with `.cpp:320-370`. Lift the `TileRenderer::RenderParams` population to a `RenderParamsBuilder`. Target: cut `render_tile_fallback` to <150 LOC.

- **P1.2** — *Collapse `render_sorted_splats_with_context` skip paths.* Effort: ~0.5d. Introduce `SkipFrameContext` with `(raster_skip_reason, composite_skip_reason, route_uid, raster_io_filler, composite_io_filler)`. Replace the seven-branch ladder with a `dispatch(skip_ctx)` call.

- **P1.3** — *Prune `PipelineFeatureSet` dead flags.* Effort: ~0.5d. Grep each `enable_*` for its actual pipeline consumer. If `enable_two_stage_sort` reaches no sort-stage branch in the stages slice, either wire it (requires collaboration with sorting orchestrator) or mark deprecated. Deleting dead flags removes ~100 LOC of provenance bookkeeping.

- **P1.4** — *Cache the lighting signature.* Effort: ~0.5d. `_compute_lighting_signature`'s ~60 `light_get_*` calls per light per frame should be folded into a `LightsFingerprint` stored on `FrameState` and invalidated when `LightStorage` version bumps + when `LIGHT_PARAM_*` inspector edits happen.

**P2 (nice):**

- **P2.1** — *Split render_pipeline_stages.cpp.* Effort: ~1d. Four files: `render_pipeline_stages_signatures.cpp` (hashing), `render_pipeline_stages_cull_sort.cpp` (CullStage/SortStage), `render_pipeline_stages_raster.cpp` (RasterStage + tile fallback), `render_pipeline_stages.cpp` (frame-plan, frame-entry, top-level flow). 2.5k → ~4 × 700 LOC is much more reviewable.

- **P2.2** — *Decouple stage structs from `GaussianSplatRenderer *`.* Effort: ~1.5d. Currently each of `CullStage`, `SortStage`, `RasterStage`, `CompositeStage` holds a raw `GaussianSplatRenderer *renderer` and calls `renderer->...` ~40 times. The `IFrameStateView`/`IFrameMutationAccess` seams are already there; converting to "stages take only the seam interfaces" would make the stages unit-testable and force the still-implicit dependencies into the interfaces.

- **P2.3** — *Format-degradation logging.* Effort: ~0.1d. Add `WARN_PRINT_ONCE` at `painterly_pass_graph.cpp:147`.

## Blind spots

- **Shader-side offset agreement.** `pipeline_io_contracts.h` `static_assert`s the CPU struct; I did not verify the matching GLSL `cluster_cull_indirect.glsl` layout. The audit rule here is "trust the offset comment + existing tests"; no cross-check code exists. Risk: shader field rename.
- **ComputeRasterPolicy semantics.** `enable_fast_raster` → `ForceOn` → `TileRenderer`; the actual TileRenderer branch on `ForceOn` vs `Default` is out of slice. Whether `ForceOn` bypasses fragment-path fallbacks correctly on AMD/Intel is not checked here.
- **GPUSorter ingestion of `SortStage` domain flipping.** The output domain check at `.cpp:1154-1163` relies on `pipeline->sort_for_view` correctly returning `SPLAT_REF` for chunk input; the actual sorter code is out of slice.
- **PainterlyPassGraph cross-device texture sharing.** The `primary_device->texture_create` + `rd->texture_create_from_extension` dance (`painterly_pass_graph.cpp:230-270`) is intricate enough that RID leak on partial failure paths is plausible; full audit of `_ensure_texture` upgrade path vs `_release_texture` is out of scope of this slice.
- **Test coverage of stage invariants.** `tests/test_renderer_pipeline.h` is referenced as a user of `publish_instance_pipeline_contract`; I did not open it to confirm what it actually checks. The rest of `ProjectSettings`-bound flag tests were similarly not read.
- **Live integration of `pipeline_feature_set` with QualityTierConfig.** `pipeline_feature_set.cpp:87-107` applies tier-preset overrides; the tier config itself and its interaction with `render_config_orchestrator` were not opened.

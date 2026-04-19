# Render Streaming Orchestrator — Deep Audit

> **Unit 11** — Render orchestrator B (streaming path).
> Scope: `modules/gaussian_splatting/renderer/render_streaming_orchestrator.{cpp,h}`.
> Method: read both files end-to-end (header 113 LOC; `.cpp` 2,303 LOC read in two ≥1,000-line chunks + targeted re-reads). All cited line numbers verified against the tree at worktree `agent-af20bde6` (branch `claude/determined-nightingale-a51a3e`).

---

## Summary

**Grade: D+**

The streaming orchestrator is the textbook example of a "polite god-class." The *class* itself (in the `.h`) declares only 9 methods and ~20 members — which is genuinely modest, and had this been executed faithfully, it would deserve a B or better. The reality is in the `.cpp`: `render_streaming_frame` alone is **817 lines** (L1356–L2172) and `sync_instance_pipeline_assets` is **453 lines** (L901–L1354). Between them they do readiness arbitration, fingerprinting, owner-mismatch remediation, static-layout hint validation (with its own ad-hoc enum taxonomy), per-frame GPU buffer allocation (six distinct storage buffers), sort-capacity handoff, fallback-instance injection, invariant-violation routing, and three overlapping flavors of diagnostic metrics publication — all inline, with state captured through `[&]` lambdas layered three deep.

The header contract is RuntimePorts-based and looks like a clean port/adapter seam — nine `T (GaussianSplatRenderer::*)(…)` member function pointers injected at construction (`render_streaming_orchestrator.h:16-32`). That is genuinely good. But the class then reaches **around** the ports and calls `renderer->publish_instance_pipeline_contract(...)`, `renderer->free_owned_resource(...)`, `renderer->track_resource_owner(...)`, `renderer->get_resource_owner(...)`, `renderer->forget_resource_owner(...)`, `renderer->get_instance_pipeline_buffers()`, `renderer->clear_instance_pipeline_buffers()`, `renderer->instance_pipeline_buffers_valid`, `renderer->instance_pipeline_buffers`, `renderer->world_submission_contract_active`, `renderer->reset_legacy_streaming_data_path_state()` directly. So the nine-port contract is a fig leaf; the real coupling surface is the entire renderer public + package-private API.

Cohesion is **poor**. This class owns:
1. Streaming **bootstrap retry** state (L80-81, L791).
2. **Asset snapshot caching** across frames (L71-75, L633).
3. **Static chunk layout hint** validation with its own failure-reason/category taxonomy (L166-236, L900-1284).
4. **GPU buffer lifecycle** for six instance-pipeline buffers (L1711-1816).
5. **Instance pipeline contract** fingerprint/upload arbitration (L46-115, L1850-1890).
6. **Readiness + invariant routing** for downstream diagnostics (L347-451, L1989-2035).
7. **Owner-mismatch remediation** (handle → owner RD mismatch) (L473-555).
8. **Fallback-instance injection** (L1544-1573).
9. **VisibleLOD selection** cache (L677-707).
10. **Streaming-only tick** (no render) — a near-duplicate of the render path (L2174-2303).

Those are at least six separate concerns. Pack them into one "orchestrator" and the orchestrator becomes the *only* place where any of it can be understood — which is exactly where we are.

Back-pressure at the renderer boundary is **not robust**. The 30-frame bootstrap cooldown (`kStreamingBootstrapRetryCooldownFrames`, L120) is the only throttle; there is no credit/budget loop, no eviction pressure signal coming back from the GPU, and the sort-capacity handoff at L1823-1838 silently clamps `max_visible_splats` with a one-shot warning that does not propagate to the metrics dictionary. Residency publication happens through `_build_streaming_instance_asset_remap` (L122-164) every frame regardless of whether the remap changed, and the fingerprint that gates upload (L1859) does not include the instance *data* — only its count and the remap — so a per-instance transform change would need to be caught by the upstream generation counter (`instance_generation` from the director), which is a fragile invariant-across-classes.

---

## What this code does

`RenderStreamingOrchestrator` is the per-frame driver for the **streaming** instance-rendering path (as opposed to the resident path) of the Gaussian splat renderer. Owned by `GaussianSplatRenderer`, called from `render_scene_instance` (confirmed at `renderer/gaussian_splat_renderer.cpp:2274`), it:

1. Decides whether a `GaussianStreamingSystem` should be created (policy-gated by `FrameBackendPlan::prefer_resident_backend`) — `ensure_instance_streaming_system` L791.
2. Walks `GaussianSplatSceneDirector` each frame to collect which assets are visible, with an asset-generation cache — `refresh_instance_asset_snapshot` L633; `produce_visible_lod_selection` L677.
3. Pushes residency requests to the streaming system for the visible LODs — `consume_visible_lod_selection_for_residency` L709; `_collect_instance_pipeline_residency_requests` L557.
4. Publishes static chunk layout hints (and a second "primary" remap) to the streaming system with extensive validation — `sync_instance_pipeline_assets` L901 (massive).
5. Allocates six GPU storage buffers (visible-chunks, splat-refs, counters, chunk-dispatch, indirect-count, instance-count) sized from working-set × instance count × per-chunk splats — L1711-1816.
6. Cross-checks that buffer ownership matches the active `RenderingDevice` and invalidates stale handles — `_remediate_instance_pipeline_buffer_owner_mismatch` L473.
7. Publishes the `InstancePipelineContract` to the renderer, gated by a 64-bit fingerprint over (resource RIDs × capacities × remap) — L1850-1889.
8. Decides "ready / not-ready" across atlas, cull, sort, raster sub-contracts; emits diagnostics; routes `route_uid` to the debug state — L1891-2099.
9. Invokes the actual cull/sort/raster pipeline through the `run_cull_sort_pipeline_frame` runtime port — L2162.
10. Separately, `tick_streaming_only` (L2174) runs the streaming update without rasterization — used by non-rendering ticks (e.g., server-side LOD warmup, world-submission paths).

Plus one utility: `should_throttle_streaming_rebuild` (L745) — a small policy that prevents cache rebuild thrashing when many small chunk changes land back-to-back.

---

## File anatomy

| Range        | LOC   | Section                                                                                  | Notes |
|--------------|-------|------------------------------------------------------------------------------------------|-------|
| L1–L21       | 21    | Includes + usings                                                                        | Pulls in 12 headers; `RenderDataOrchestrator`, `RenderDeviceOrchestrator`, `RenderPipelineStages`, contracts × 3. |
| L22–L30      | 9     | `using` aliases for 7 nested types from `GaussianSplatRenderer`                          | Signals how many renderer-internal types leak here. |
| L32–L45      | 14    | `_mix_*_generation` 64-bit hash helpers                                                  | Pure. Belongs in a shared `fingerprint.h`. |
| L46–L115     | 70    | `_compute_instance_pipeline_resource_fingerprint`, `_compute_instance_asset_remap_fingerprint`, `_compute_instance_pipeline_upload_fingerprint` | 26 RIDs + 11 u32s mixed. Allocates an intermediate `LocalVector<uint32_t>` every call (L90). |
| L117–L120    | 4     | Four module-private `constexpr`                                                          | `kStreamingBootstrapRetryCooldownFrames=30` is the *only* retry throttle. |
| L122–L164    | 43    | `_build_streaming_instance_asset_remap`                                                  | Called every frame; builds a `HashMap<u32,u32>` and returns it by value. |
| L166–L190    | 25    | 3 enums: `LayoutHintValidationUsage`, `LayoutHintFailureReason`, `LayoutHintFailureCategory` | Hint validation has its own taxonomy. |
| L191–L263    | 73    | 4 stringifiers + `_layout_hint_failure_detail`                                           | Pure mapping; 30+ lines of switch cases. |
| L265–L345    | 81    | `_apply_static_layout_fallback_diagnostics`                                              | Publishes **the same** fallback stats to **three** parallel metrics dictionaries. |
| L347–L405    | 59    | `StreamingReadinessState` + token/reason stringifiers                                    | Own taxonomy parallel to the layout-hint one. |
| L408–L452    | 45    | `_apply_streaming_render_readiness_diagnostics`                                          | Duplicates state+reason into top-level metrics, `render_readiness` dict, AND `diagnostics` dict. |
| L454–L471    | 18    | Two contract/debug predicates                                                            | `_is_impossible_streaming_activation_violation` is trivially "instance_count>0 && has_violation" — borderline misleading name. |
| L473–L555    | 83    | `OwnerMismatchRemediationResult` + `_remediate_instance_pipeline_buffer_owner_mismatch`  | Defensive rebind logic; if contract returns bad decision, it force-overrides (L505-506). |
| L557–L596    | 40    | `_collect_instance_pipeline_residency_requests`                                          | Builds LOD masks from instances, emits 1 residency request per (asset, LOD) bit. Reference semantics: `r_request_count` can be null. |
| L598–L614    | 17    | Constructor                                                                              | 9 `ERR_FAIL_*` validations of runtime ports — good. |
| L616–L631    | 16    | `invalidate_instance_pipeline_caches`                                                    | Resets ~9 fields. Not called from `ensure_instance_streaming_system`'s failure paths — only on success and on sync-assets failure. |
| L633–L675    | 43    | `refresh_instance_asset_snapshot`                                                        | Asset-generation cache; re-sorts `InstanceAssetRegistration` by asset_id when >1 entry. |
| L677–L707    | 31    | `produce_visible_lod_selection`                                                          | Calls `update_instance_lods_for_renderer` then builds the instance buffer. Cache keyed on (generation, shadow_only). |
| L709–L743    | 35    | `consume_visible_lod_selection_for_residency`                                            | Begin / collect / finalize residency request pattern. |
| L745–L789    | 45    | `should_throttle_streaming_rebuild`                                                      | **Correct and well-commented** — a rare standout. Frame-based hysteresis with explicit "never throttle loads" rule. |
| L791–L899    | 109   | `ensure_instance_streaming_system`                                                       | Bootstrap with 30-frame retry cooldown; invalidates 15+ cache fields on success. |
| L901–L1354   | 454   | `sync_instance_pipeline_assets`                                                          | **Monster method.** 4 nested lambdas, 2 separate layout-hint validators (primary + IO), transient-empty grace streak, register/unregister loop. |
| L1356–L2172  | 817   | `render_streaming_frame`                                                                 | **The god-method.** 4 nested lambdas (`publish_not_ready_route`, `log_streaming_reset`, `set_instance_invariant_status`, `finalize_streaming_frame`, `remediate_instance_pipeline_buffer`, `emit_invariant_violation_diagnostic`). |
| L2174–L2303  | 130   | `tick_streaming_only`                                                                    | Mirrors `render_streaming_frame`'s first half; independent copy of `sync → produce → consume → update_streaming → metrics`. |

---

## Strengths

1. **RuntimePorts dependency-injection pattern (header L16-32).** Nine engine method pointers captured at construction, validated with `ERR_FAIL_COND_MSG` in the ctor (L606-613). The shape is right: the orchestrator *could* be testable in isolation. That the class then reaches around it is a separate sin.

2. **`should_throttle_streaming_rebuild` (L745-789)** is genuinely good. The comment at L761-772 ("never throttle when chunks have JUST loaded") captures a real bug that used to exist and explains why the fix must look the way it does. Keep this function; promote it to a named policy.

3. **Fingerprint-gated upload (L1859-1862).** The upload path is skipped when (generation, fingerprint) both match — correct strategy for avoiding redundant GPU uploads.

4. **Owner-mismatch contract (L490-506).** Calls `ResourceOwnerMismatchContract::evaluate + validate` and overrides the decision to the safe side on contract violation. This is defense-in-depth done right, even if it's in the wrong file.

5. **Explicit readiness taxonomy.** `StreamingReadinessState` (L347-356) makes the not-ready reasons enumerable and routable. Better than a bool + string combo.

6. **Asset-snapshot generation cache (L633-674)** short-circuits repeated director scans within a frame if generation hasn't changed — appropriate.

7. **Transient-empty grace window (L922-934)** prevents a single frame of empty asset registrations from thrashing the registered-asset set. Demonstrates real operational experience.

8. **30-frame bootstrap retry cooldown (L120, L818).** At least *some* back-pressure against a failing bootstrap storm.

9. **Per-LOD residency request fan-out (L587-594)** is bit-mask driven — compact, avoids duplicate requests per asset.

10. **Constructor port validation** with 9 distinct messages (L606-613) — when a port is missing you get told exactly which one.

---

## Top issues

1. **[maint] `render_streaming_orchestrator.cpp:1356-2172` — `render_streaming_frame` is 817 LOC with 4 nested `[&]` lambdas.** This is the headline anti-pattern. `finalize_streaming_frame`, `publish_not_ready_route`, `log_streaming_reset`, `set_instance_invariant_status`, `remediate_instance_pipeline_buffer`, `emit_invariant_violation_diagnostic` are all captured lambdas with `[&]` capture — they close over the entire local frame including `streaming_state`, `resource_state`, `buffers`, fingerprints, and counters. You cannot unit-test any of them. You cannot reason about data flow without re-reading all 817 lines because any lambda can touch any variable. **Fix direction:** split into an explicit `FrameContext` struct + free functions `FinalizeMetrics(FrameContext&)`, `PublishReadinessRoute(FrameContext&, State)`, `RemediateBufferOwners(FrameContext&)`. 30-50 LOC each. Worth the 1-day refactor.

2. **[maint] `render_streaming_orchestrator.cpp:901-1354` — `sync_instance_pipeline_assets` is 454 LOC with inline primary-layout validation, IO-layout validation, register/unregister diff, AND chunk-range overlap sort.** The two validators (L1030-1101 and L1113-1188) are structurally parallel but coded independently. **Fix direction:** extract a `StaticLayoutHintValidator` with a single `Validate(ChunksView, Usage) -> LayoutHintValidationResult` entry point. Both primary and IO paths then call it. Saves ~200 LOC and unifies the failure taxonomy.

3. **[crash] `render_streaming_orchestrator.cpp:939` — `state_view.get_subsystem_state_view().gpu_culler->get_state()` dereferences `gpu_culler` without a null check.** The pipeline could plausibly call `sync_instance_pipeline_assets` before the GPU culler has been bootstrapped (e.g., first frame on editor startup, or after a device recreation). Every other subsystem pointer in this file is null-checked (e.g., `streaming_system` L905, `director` L913, `rd` L821) — `gpu_culler` is the odd one out. **Fix direction:** `const GaussianSplatting::GpuCuller *gpu_culler = state_view.get_subsystem_state_view().gpu_culler; if (!gpu_culler) { invalidate_instance_pipeline_caches(); return; }`.

4. **[perf] `render_streaming_orchestrator.cpp:81-104` — `_compute_instance_asset_remap_fingerprint` allocates a `LocalVector<uint32_t>` and calls `std::sort` every frame, for every remap.** Called from L1859 (always) and L1888 (when atlas ready, always). The `asset_to_dense_id` map is typically small (≤ asset count), but the allocation churn is pointless: the remap is built right above by `_build_streaming_instance_asset_remap`; carry a pre-sorted vector alongside the HashMap. **Fix direction:** change `PublishedInstanceAssetRemap` to carry a `LocalVector<Pair<u32,u32>> sorted_pairs` that is populated at build time. Zero-alloc fingerprint after that.

5. **[maint] `render_streaming_orchestrator.cpp:80-81 (header), L815-875 (.cpp)` — `streaming_bootstrap_last_error` is written 6 times and read zero times.** Grep confirms: no `get_streaming_bootstrap_last_error()`, no metrics publication, no diagnostics dict entry. It's a field that exists only to satisfy the author's conscience. **Fix direction:** either publish it into `streaming_state` metrics (one-liner) or delete it. Right now it's pure noise.

6. **[corruption] `render_streaming_orchestrator.cpp:1691-1698` — sort-capacity clamp under the global `g_gpu_sorting_config.max_sort_elements` can silently reduce `max_visible_splats` below what the instance-pipeline contract requires.** If the clamp kicks in, the fingerprint at L1854 changes, which is fine, but the *contract* at L1870 (`publish_instance_pipeline_contract`) is called with the clamped buffers — meaning downstream consumers see a different splat budget than they would absent the global config. No warning is emitted when the clamp fires (unlike the `WARN_PRINT_ONCE` at L1836 for the per-sorter-instance clamp). **Fix direction:** when `max_visible_splats_u64 > sort_cap`, emit a one-shot diagnostic field in `streaming_metrics` so the UI can show "capped by global sort config."

7. **[perf] `render_streaming_orchestrator.cpp:1744, 1762, 1776, 1788, 1801, 1812` — six separate `storage_buffer_create` calls in the "allocate if missing" blocks, each followed by `set_resource_name` + `track_resource_owner`.** No batching; each call takes a lock on the RD. For a scene with 10 instances this is 60 RD round-trips per buffer-rebuild frame. Worse, there's no RAII on the allocate-and-name paths: if `set_resource_name` fails (it won't, but still), the buffer handle is tracked anyway. **Fix direction:** a `CreateTrackedStorageBuffer(rd, size, name, usage=0)` helper that does the triple in a single call. Reduces noise and centralizes error handling.

8. **[perf] `render_streaming_orchestrator.cpp:1530-1537` — `instance_pipeline_instance_cache = selection.get_instances();` copies a `LocalVector<InstanceDataGPU>` every frame.** `InstanceDataGPU` is ~96 bytes (16 floats + 2 u32s + 2 u32s + 4 floats). For 1000 instances that's 96 KB copied per frame just to cache it into a member. The source (`VisibleLODSelection::instances`) is already a member; the copy buys nothing. **Fix direction:** hold a pointer or index into `visible_lod_selection.instances`, not a copy. Or move the cache field out entirely — `render_streaming_frame` doesn't need it to persist across frames, the instance buffer upload happens within the frame.

9. **[corruption] `render_streaming_orchestrator.cpp:1993-1998` — `static bool emitted_reasons[COUNT] = {}` inside a method, guarded by reason index.** This is function-local static state that is *process-wide* and *not thread-safe for write*. Multiple renderer instances (editor preview + game preview) share it. Once a violation fires for a reason, it's muted forever until process exit. **Fix direction:** move to a per-orchestrator member `BitField<COUNT>`. Makes test reproducibility possible (each test run starts fresh), and allows a reset hook for development.

10. **[crash] `render_streaming_orchestrator.cpp:2037-2069` — hard-fail path under `DEBUG_ENABLED || TESTS_ENABLED` calls `ERR_FAIL_V_MSG` *after* mutating `renderer->instance_pipeline_buffers_valid = false` and finalizing metrics.** `ERR_FAIL_V_MSG` in Godot will return from the current function — but the *caller* (`render_scene_instance` in `gaussian_splat_renderer.cpp:2274`) does not know the frame is half-finalized. In tests, this means the next frame re-enters with partially-invalidated state. **Fix direction:** reverse the order — run the hard-fail first, *then* mutate and finalize only if we're continuing. Or, more robustly, make this not a hard-fail in TESTS_ENABLED (tests can assert the readiness state directly, no need to crash).

11. **[maint] `render_streaming_orchestrator.cpp:265-345` — `_apply_static_layout_fallback_diagnostics` publishes the same 10 fields to **three** dictionaries: `layout_hint_orchestrator_validation`, `layout_hint_validation` (merging with existing), and the top-level `streaming_metrics` keys prefixed `layout_hint_orchestrator_*`.** That's a 3× blow-up in metrics cardinality for zero information gain. The consumer (UI?) reads one of them, the other two are dead weight on the Dictionary hash map. **Fix direction:** pick the canonical location (the nested dict), delete the other two, adjust the one consumer.

12. **[maint] `render_streaming_orchestrator.cpp:408-451` — `_apply_streaming_render_readiness_diagnostics` likewise duplicates readiness state into `render_readiness` dict, top-level `render_readiness_*` keys, AND `diagnostics` dict when not-ready.** Same criticism. Same fix.

13. **[maint] `render_streaming_orchestrator.cpp:2174-2303` — `tick_streaming_only` is a near-duplicate of `render_streaming_frame`'s setup phase.** `sync → produce → consume → update_streaming` is copy-pasted; metrics publication is copy-pasted (with slightly different content); the "instances but no registered assets" reset is copy-pasted. Divergence *will* happen and has probably already happened — the render path calls `finalize_streaming_frame` (which publishes static-layout-fallback diagnostics) while the tick path has its own `_apply_static_layout_fallback_diagnostics` call (L2247). If one is fixed, the other won't be. **Fix direction:** extract `struct StreamingFrameSetup { ... }` with a `Prepare(FrameBackendPlan, Transform, Projection)` method; both entry points call it.

14. **[maint] `render_streaming_orchestrator.cpp:1629-1660, 1711-1816` — direct field access `renderer->instance_pipeline_buffers`, `renderer->world_submission_contract_active`, `renderer->instance_pipeline_buffers_valid`.** The RuntimePorts contract advertises 9 methods but the class reaches through to ~7 more fields/methods directly. Either these are part of the contract or they aren't. **Fix direction:** add `get_instance_pipeline_buffers_mut`, `set_instance_pipeline_buffers_valid`, `is_world_submission_contract_active` as ports — or move this class inside the renderer compilation unit and stop pretending there's a boundary.

15. **[perf] `render_streaming_orchestrator.cpp:1361-1366` — function-static `streaming_frame_counter` mod 60 gating trace emission.** Process-wide shared state (see issue #9). Two editor viewports sharing a single `RenderStreamingOrchestrator` (unlikely but possible) interleave and produce skewed trace cadence. **Fix direction:** per-instance member; also respect the trace category filter.

16. **[perf] `render_streaming_orchestrator.cpp:1648-1655` — `static int instance_buffer_diag_counter` capped at 20 for the lifetime of the process.** After the first 20 frames of tracing, atlas handoff diagnostics are muted forever. The author clearly meant "only spam for the first 20 frames after startup" but there's no reset on streaming-system recreation. **Fix direction:** per-instance counter reset on `ensure_instance_streaming_system` success path.

---

## Cross-cutting patterns

- **Lambda capture overuse.** Every major method uses `[&]` lambdas that close over frame-local state. This is the author's "poor-man's method extraction" — but without the benefit of unit-testability, named parameters, or visible data flow. The `.cpp` has at least 10 such lambdas. They are the single biggest readability killer.

- **Parallel diagnostic publication.** Four separate "apply diagnostics" functions (`_apply_static_layout_fallback_diagnostics`, `_apply_streaming_render_readiness_diagnostics`, inline metrics dictionary mutations in `finalize_streaming_frame`, inline mutations in `tick_streaming_only`). Each one duplicates information across 2-3 keys in the same Dictionary. This is a sign that nobody owns the metrics schema — it has grown ad-hoc and nobody has the courage to rip out duplicates.

- **Port contract leakage.** `RuntimePorts` is the public face; the private reality is 7+ direct accesses to renderer internals. The contract would be defensible if it were either (a) strict — all renderer access through ports — or (b) absent — the orchestrator is a renderer helper, live in the renderer TU. The current middle ground is worse than either.

- **Two independent failure taxonomies.** `LayoutHintFailureReason` (L171) has 7 values with category mapping (L219). `StreamingReadinessState` (L347) has 8 values with token/reason/route mapping. Both have stringifier functions, both get published to metrics. They are philosophically aligned but mechanically separate. A shared `EnumStringify` template would halve the code.

- **Fingerprint proliferation.** Three distinct fingerprint functions (resource, remap, upload). The upload fingerprint *includes* the remap fingerprint (L113). The content generation mixes remap+atlas generations without the fingerprint. There are now 5 names for "is this frame's pipeline state different from last frame's?" and they are not all equivalent.

- **Static mutable state for rate-limiting diagnostics.** `streaming_frame_counter`, `instance_buffer_diag_counter`, `emitted_reasons[]`. All three are function-local statics. All three are fragile under test re-entry, multiple renderer instances, or re-initialization.

- **No GPU back-pressure.** The only throttle is the 30-frame bootstrap retry. Once bootstrapped, the orchestrator assumes the streaming system and sorting pipeline can absorb whatever `max_visible_splats` is thrown at them. The sort-capacity clamp (L1835-1837) is reactive (warns after the fact); there's no "we're behind, shrink the working set" signal.

---

## Proposed split seams

Five concrete seams, from lowest to highest risk. Each seam below is achievable without a fundamental architectural rethink.

### Seam 1 — Pure helpers → `renderer/streaming_fingerprint.h` (LOW RISK)

Extract lines L32-L115 (`_mix_*`, `_compute_*_fingerprint`) into a header-only `streaming_fingerprint.h`. They are pure functions with no member dependencies. 100 LOC moved, no behavior change, immediately unit-testable.

### Seam 2 — Layout-hint validation → `renderer/static_layout_hint_validator.{h,cpp}` (LOW RISK)

Extract L166-263 (enums + stringifiers) + the two validation blocks inside `sync_instance_pipeline_assets` (L1028-1097 and L1103-1195). Define:

```cpp
struct LayoutHintValidationResult {
  bool valid = false;
  LayoutHintFailureReason reason = LayoutHintFailureReason::NONE;
  int hint_index = -1;
  uint64_t detail_a = 0;
  uint64_t detail_b = 0;
  Vector<ChunkLayoutHint> hints;          // populated when valid
  Vector<uint32_t> source_indices;        // populated for PRIMARY when valid
};

class StaticLayoutHintValidator {
  LayoutHintValidationResult validate_primary(ChunksView, u32 primary_splat_count);
  LayoutHintValidationResult validate_io(ChunksView);
};
```

Unifies the two validators (~200 LOC saved), gives the failure counters a single home, makes the fallback-diagnostics publication a single caller. `sync_instance_pipeline_assets` drops to ~150 LOC.

### Seam 3 — Diagnostic publication → `renderer/streaming_metrics_publisher.{h,cpp}` (MEDIUM RISK)

Extract L265-345 (`_apply_static_layout_fallback_diagnostics`), L408-451 (`_apply_streaming_render_readiness_diagnostics`), and the inline finalize-streaming-frame metrics dictionary mutations (L1472-1497, L2246-2302). Define:

```cpp
class StreamingMetricsPublisher {
  void publish_static_layout_fallback(Dictionary &metrics, const StaticLayoutFallbackState &);
  void publish_render_readiness(Dictionary &metrics, StreamingReadinessState, const String &detail);
  void publish_stage_timings(Dictionary &metrics, const StageMetrics &);
  void publish_invariant_status(Dictionary &metrics, const InstanceInvariantStatus &);
};
```

Unblocks issue #11 (the 3× duplication): the publisher picks one canonical location, retires the other two. Moves ~250 LOC of diagnostic plumbing out of the orchestrator. Risk: any UI reading the legacy duplicate keys will break — audit before deleting.

### Seam 4 — GPU buffer lifecycle → `renderer/instance_pipeline_buffer_allocator.{h,cpp}` (MEDIUM RISK)

Extract L473-555 (`_remediate_instance_pipeline_buffer_owner_mismatch`) and L1711-1816 (the six "allocate if missing" blocks). Define:

```cpp
class InstancePipelineBufferAllocator {
  InstancePipelineBufferAllocator(GaussianSplatRenderer *, RenderingDevice *);

  // Remediates stale owner + allocates if missing. Returns OwnerMismatchReport.
  struct Request {
    uint32_t max_visible_chunks;
    uint32_t max_visible_splats;
  };
  struct Report {
    uint32_t owner_mismatch_detected = 0;
    uint32_t owner_mismatch_forced_invalidation = 0;
  };
  Report ensure(Request, ResourceState &, InstancePipelineBuffers &);
};
```

Isolates the *eleven* `renderer->free_owned_resource / track_resource_owner / get_resource_owner / forget_resource_owner` touch-points to this one file. Reduces the orchestrator's renderer coupling by ~60%. `render_streaming_frame` drops another ~200 LOC.

### Seam 5 — Setup phase shared with tick-only → `StreamingFrameSetup` (MEDIUM-HIGH RISK)

Extract the code that is currently duplicated between `render_streaming_frame` (L1499-1595) and `tick_streaming_only` (L2208-2241):

```cpp
struct StreamingFrameSetup {
  enum class Result { Ready, ResetRequired, NotReady };
  Result prepare(const FrameBackendPlan &, const Transform3D &, const Projection &,
                 ResetReason *, String *readiness_detail);
  GaussianStreamingSystem *streaming_system = nullptr;
  LocalVector<InstanceDataGPU> *visible_instances = nullptr;
};
```

Both callers drop 100+ LOC of duplicated setup. Unifies the "instances without registered assets" reset logic.

After all five seams: `render_streaming_frame` shrinks from 817 → ~250 LOC; `sync_instance_pipeline_assets` shrinks from 454 → ~150 LOC; the orchestrator becomes a thin driver calling named collaborators. That *is* the promise the class name makes.

---

## Recommended refactor moves

### P0 (before the next release — high-value, low-risk)

| # | Move                                                             | Effort   | Risk |
|---|------------------------------------------------------------------|----------|------|
| 1 | **Issue #3 null-check.** Guard `gpu_culler->get_state()` at L939 | 5 min    | none |
| 2 | **Issue #5 cleanup.** Publish or delete `streaming_bootstrap_last_error` | 15 min | none |
| 3 | **Issue #8 copy elision.** Replace `instance_pipeline_instance_cache = selection.get_instances()` with a pointer | 30 min | low |
| 4 | **Issue #10 hard-fail ordering.** Move `ERR_FAIL_V_MSG` before state mutations in the TESTS path | 30 min | low |
| 5 | **Seam 1.** Extract fingerprint helpers to `streaming_fingerprint.h` | 1 hour | none |

### P1 (next 1–2 sprints — medium effort, high value)

| # | Move                                                             | Effort   | Risk |
|---|------------------------------------------------------------------|----------|------|
| 6 | **Seam 2.** `StaticLayoutHintValidator`                          | 1 day    | low (pure refactor, same inputs/outputs) |
| 7 | **Seam 4.** `InstancePipelineBufferAllocator`                    | 2 days   | medium (touches GPU resource lifetime) |
| 8 | **Issue #4 perf.** Pre-sorted remap pairs to avoid per-frame alloc+sort | 2 hours | low |
| 9 | **Issue #9.** Move `emitted_reasons[]` to a per-instance BitField | 30 min | none |
| 10| **Issue #7.** Introduce `CreateTrackedStorageBuffer` helper       | 1 hour | none |

### P2 (next quarter — high effort, durable value)

| # | Move                                                             | Effort   | Risk |
|---|------------------------------------------------------------------|----------|------|
| 11 | **Seam 3.** `StreamingMetricsPublisher`, retire duplicate keys  | 2 days (+1 day metrics audit) | medium (downstream consumers) |
| 12 | **Seam 5.** `StreamingFrameSetup` shared by render + tick paths  | 2 days  | medium (logic convergence is semantically observable) |
| 13 | **Decide on RuntimePorts.** Either strict (all renderer access via ports) or delete the pretense | 3 days | medium |
| 14 | **Introduce GPU back-pressure.** A credit/budget signal from the sort/cull completion back to the streaming working-set sizing | 1 week | high (perf-sensitive) |

---

## Blind spots

- **`GaussianStreamingSystem` internals.** I verified the orchestrator *calls* `begin_frame / register_asset / set_primary_chunk_layout / set_io_chunk_layout_hints / request_asset_residency / update_streaming / end_frame / get_streaming_analytics`, but I cannot tell from this unit whether those calls are idempotent, thread-safe, or safe to interleave with the `tick_streaming_only` variant from a different thread. That's Units 3/4/5 territory.

- **`RenderDataOrchestrator` and `RenderDeviceOrchestrator`.** Both are constructor-injected (L599-601) but only `data_orchestrator` is used (L831-832, `get_streaming_config_overrides()`). `device_orchestrator` is stored and never dereferenced — dead dependency, or future use? Can't tell without reading Units for those orchestrators.

- **`GaussianSplatSceneDirector::get_singleton()`.** Called three times (L912, L1508, L1599, L2210). If the singleton is a global, this is the third shared-mutable-state entry point in this file (after the static function-locals and the global sorting config). Concurrency model is out-of-slice.

- **`InstancePipelineContract` evaluators.** `first_atlas_violation / first_cull_violation / first_sort_violation / first_raster_violation / evaluate_streaming_activation` are called but I haven't verified their own preconditions. A precondition violation there would surface here as spurious "not ready" states.

- **`g_gpu_sorting_config.max_sort_elements` (L1691).** Process-global. Who writes it? When? If the config is reloaded mid-run, the fingerprint doesn't include it, so the orchestrator's cache-gated upload would miss the change. Out-of-slice but flagged.

- **Test coverage.** Did not enumerate tests. The `TESTS_ENABLED` branch at L2037-2069 changes runtime behavior; I cannot confirm there's a test that actually exercises it without running `scons tests=yes`.

- **Shadow-instance path.** `renderer->is_shadow_instance_filter_enabled()` gates caching (L641, L693). The cache-key composition is (generation, shadow_only). Correct as long as the shadow filter is stable within a frame — I have not verified that.

- **`world_submission_contract_active` (L1659).** Read-only usage here. Its write-site is in the main renderer. If it flips mid-frame (shouldn't happen, but who enforces?), the published contract has stale semantics.

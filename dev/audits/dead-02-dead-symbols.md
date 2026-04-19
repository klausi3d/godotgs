# Dead Symbols — Deep Sweep

## Summary

The Gaussian Splatting Godot module carries a substantial layer of declared-but-unreachable
code: header methods with no corresponding definitions (linker bombs the moment the caller is
compiled), defined-but-uncalled methods, write-only fields, constant fields, and GDCLASS-marked
types whose `_bind_methods()` register ClassDB entries that are never reachable because the
class itself is never `GDREGISTER_CLASS`'d.

- **A. Zero-caller functions / methods**: 12 confirmed
- **B. Declared-never-defined (linker bombs)**: 8 confirmed (5 in `gpu_memory_stream.h`, 3 in `gaussian_import_dialog.h`)
- **C. Defined-never-called (internal linkage)**: 0 confirmed under this lens (rolls into A)
- **D. Write-only fields**: 3 confirmed
- **E. Constant (never-written) fields**: 1 confirmed (`StreamingStats::defrag_count`)
- **F. Unused enum cases**: 0 confirmed (seed was incorrect — reasons *are* surfaced via `get_violation_reason_name`)
- **G. Unused types**: 2 confirmed (`GPUBufferManager::SortKey`, `CoarseSyncPolicy`)
- **H. GDCLASS types not GDREGISTER_CLASS'd**: 11 production (+ test + editor) — several carry `bind_method` payloads that are unreachable
- **I. Bind-method orphans (unreachable bindings via unregistered classes)**: 10 classes with `_bind_methods` whose class is not registered

Several prior-audit seeds were verified *false* under current line numbers — see
"Things that LOOK dead but aren't" for reviewer hand-off.

## Method

- Primary sweep: `Grep -t cpp -t h` the bare identifier across `modules/gaussian_splatting/`,
  read the hit count, then inspect ambiguous cases by looking at the call context.
- For fields: separate writes from reads using `pattern = field_name` and categorising each
  hit as `field =` (write) vs `= field` / `field\s*[^=]` (read). When ambiguous I opened
  the file.
- False-positive guards applied:
  - `callable_mp(this, &Class::method)` — counts as a reference (function-pointer use).
  - `ClassDB::bind_method(..., &Class::method)` — counts as a reference (reflection).
  - `override` methods — only flagged when both the base-class call site AND any test call
    site are both zero.
  - Template-only / friend-only uses — I verified the include graph by reading the declaring
    header when in doubt.
  - Function name in `WARN_PRINT("... func_name ...")` literals — NOT counted as a call.

Hit counts below were captured on the worktree at HEAD `claude/determined-nightingale-a51a3e`.

## A. Zero-caller functions/methods

1. `modules/gaussian_splatting/lod/hierarchical_splat_structure.cpp:492` —
   `float HierarchicalSplatStructure::calculate_importance(const SplatInfo&, const OctreeNode*) const`
   — evidence: `grep "calculate_importance" -> 2 hits` (decl + defn only, no call sites)
   — recommendation: **DELETE** (26 LOC, computes importance nothing reads).

2. `modules/gaussian_splatting/renderer/painterly_pass_graph.h:90` —
   `const Vector<PassNode> &PainterlyPassGraph::get_passes() const` (inline)
   — evidence: `grep "pass_graph.(get_passes|find_pass)|graph->(get_passes|find_pass)" -> 0 hits`
   — `PainterlyRenderer::execute_painterly_passes` uses the hard-coded sobel→brush path
     (`_execute_sobel_pass` / `_execute_brush_pass`), never enumerates `passes`.
   — recommendation: **DELETE** (rips out the unused `Vector<PassNode> passes` + `_rebuild_passes` +
     `PassNode` / `PassId` / `PassType` — whole pass-graph layer is dormant).

3. `modules/gaussian_splatting/renderer/painterly_pass_graph.cpp:126` —
   `const PainterlyPassGraph::PassNode *PainterlyPassGraph::find_pass(PassId p_id) const`
   — evidence: `grep "find_pass" -> 2 hits` (decl + defn only)
   — recommendation: **DELETE** together with (2).

4. `modules/gaussian_splatting/renderer/gpu_sorter.cpp:2391` —
   `uint64_t RadixSort::sort_async_with_timeline(RID, RID, uint32_t, RID, uint64_t, RID, uint64_t)`
   — evidence: `grep "sort_async_with_timeline" -> 3 hits` (decl + defn + WARN_PRINT literal only)
   — Also surfaces a correctness concern: `_sort_async_internal` accepts the wait/signal
     semaphore arguments at `gpu_sorter.cpp:1303` but never passes them to any RD submission call —
     "timeline" is advertised, not applied.
   — recommendation: **DELETE** the public method; if timeline semantics are desired, re-add when an
     implementation actually exists.

5. `modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp:1703` —
   `bool GaussianSplatRenderer::render_directional_shadow_map(...)`
   — evidence: `grep "render_directional_shadow_map" -> 6 hits` (all self-refs in decl/defn/WARN_PRINTs)
   — recommendation: **DELETE** (≈80 LOC + its helpers `_ensure_shadow_blit_resources`,
     `_blit_shadow_depth`, `_ensure_shadow_output_compositor` — check each separately before removing).

6. `modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp:1343` —
   `int GaussianSplatRenderer::get_cached_streaming_route_policy()`
   — evidence: `grep "get_cached_streaming_route_policy\b" -> 4 hits` (decl + defn × 2, self-ref in
     `get_cached_streaming_route_policy_source`)
   — recommendation: **DELETE** (internal caller `_refresh_streaming_route_policy_cache` is reached
     through `render_data_orchestrator.cpp:591-592` directly).

7. `modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp:1348` —
   `const String &GaussianSplatRenderer::get_cached_streaming_route_policy_source()`
   — evidence: same as (6) — no external callers
   — recommendation: **DELETE**.

8. `modules/gaussian_splatting/compute/compute_infrastructure.cpp:96` —
   `bool StageValidationHarness::all_valid() const`
   — evidence: `grep "all_valid" -> 2 production hits` (decl + defn); 1 test hit in
     `tests/test_compute_infrastructure.h:30`. Production harness callers at
     `gpu_sorting_pipeline.cpp` and `cluster_culler.cpp` only call `validate()` + read the
     per-stage `StageResult`, never `all_valid()`.
   — recommendation: **KEEP-WITH-COMMENT** (test-only; document as such so it isn't "promoted" to
     prod without deliberate use) OR move to a `GS_TEST_HELPER` header under `tests=yes`.

9. `modules/gaussian_splatting/compute/compute_infrastructure.cpp:105` —
   `String StageValidationHarness::summarize_failures() const`
   — evidence: same as (8); only test consumer at `test_compute_infrastructure.h:31`.
   — recommendation: same as (8).

10. `modules/gaussian_splatting/interfaces/overflow_auto_tuner.cpp:31` —
    `void OverflowAutoTuner::_bind_methods()` (the bindings inside it: `set_enabled`,
    `is_enabled`, `get_importance_threshold`, `get_tiny_splat_radius`, `reset_to_baselines`)
    — evidence: `OverflowAutoTuner` has `GDCLASS(...)` but `grep "GDREGISTER.*OverflowAutoTuner" -> 0`;
      therefore no entry exists in ClassDB and these 5 bindings are unreachable from
      GDScript / editor / `call_deferred`. C++ still calls these methods directly, so the
      *methods* are fine — the *bindings* are dead code.
    — recommendation: **DELETE the `_bind_methods()` body** (or GDREGISTER the class if reflection
      is actually wanted). See §I for the broader pattern.

11. `modules/gaussian_splatting/interfaces/sync_policy.h:130` — class
    `CoarseSyncPolicy` — evidence: `grep "CoarseSyncPolicy" -> 2 files`, hits only in
    `tests/test_integration.cpp` (138, 173, 207). No production instantiation / `Ref<>` / `new`.
    — recommendation: **DELETE** (dead aside from tests; strategy pattern without callers).

12. `modules/gaussian_splatting/renderer/gpu_memory_stream.h:229` — class
    `StreamingPipeline` (150+ LOC over `.h` + `.cpp`) — GDCLASS'd, uses `ClassDB::bind_method`
    for 10 methods, but is NOT `GDREGISTER_CLASS`'d, and no production file `memnew`s or holds
    a `Ref<StreamingPipeline>`. Only test callers (`tests/test_gpu_streaming.cpp` ×4,
    `tests/test_integration.cpp` ×2).
    See also note in `core/gaussian_splat_quality_config.h:56` explicitly saying *"not read by
    any production code path … not on the production streaming path"*.
    — recommendation: **DELETE** or move to a `tests/`-only fixture file.

## B. Declared-never-defined

These cause an ODR / linker error the first time an external translation unit `#include`s the
header AND emits a call to one of them. `tests=yes` builds are currently the trigger site.

1. `modules/gaussian_splatting/renderer/gpu_memory_stream.h:146` — `void _wait_for_upload(int)`
   — evidence: `grep "GaussianMemoryStream::_wait_for_upload\b" -> 0 hits` (only
   `_wait_for_upload_fence_value` is defined; the unqualified `_wait_for_upload` is not).
   Dead-header but no caller — *currently* benign. **DELETE declaration**.

2. `modules/gaussian_splatting/renderer/gpu_memory_stream.h:191` — `RID get_sort_keys_buffer() const`
   — evidence: no `GaussianMemoryStream::get_sort_keys_buffer` anywhere.
   **Called** by `tests/performance_benchmark.cpp:375, 398`. Since `SCsub:227` globs
   `tests/*.cpp` under `tests=yes`, this compiles, fails to link, and kills the test build.
   — recommendation: **DEFINE IT or REMOVE the callers in `performance_benchmark.cpp`** (see §G
   for status of `performance_benchmark.cpp` overall).

3. `modules/gaussian_splatting/renderer/gpu_memory_stream.h:197` — `void update_visible_range(uint32_t, uint32_t)`
   — evidence: no `GaussianMemoryStream::update_visible_range` defined. No caller found. **DELETE declaration**.
   (Note: `StreamingPipeline::update_visible_range` is a separate method, and *is* defined.)

4. `modules/gaussian_splatting/renderer/gpu_memory_stream.h:202` — `void set_lod_ranges(const LocalVector<uint32_t>&, const LocalVector<uint32_t>&)`
   — evidence: no definition, no caller. **DELETE declaration**.

5. `modules/gaussian_splatting/renderer/gpu_memory_stream.h:203` — `void update_culling_mask(const LocalVector<uint8_t>&)`
   — evidence: no definition, no caller. **DELETE declaration**.

6. `modules/gaussian_splatting/editor/gaussian_import_dialog.h:151` — `void _schedule_preview_update(bool p_immediate = false)`
   — evidence: `grep "GaussianImportDialog::_schedule_preview_update" -> 0 hits`. No caller found
   module-wide either. **DELETE declaration** (debounce layer was never wired).

7. `modules/gaussian_splatting/editor/gaussian_import_dialog.h:152` — `void _on_preview_debounce_timeout(int64_t)`
   — evidence: same. **DELETE declaration**.

8. `modules/gaussian_splatting/editor/gaussian_import_dialog.h:153` — `void _process_queued_preview(int64_t)`
   — evidence: same. **DELETE declaration**.

## C. Defined-never-called (internal)

All candidates found in this lens collapse into category A (see §A items 1, 4, 5, 6, 7).
No additional `static` / anonymous-namespace functions survive the filter.

## D. Write-only fields

1. `modules/gaussian_splatting/core/gaussian_data.h:346` — `mutable bool octree_dirty = true;`
   — evidence: `grep "octree_dirty" -> 6 hits` (1 decl, 5 assignments to `true`, 0 reads, 0
   assignments to `false`). The octree is rebuilt unconditionally whenever the requester
   touches it; this dirty-flag layer was never completed.
   — recommendation: **DELETE field and all 5 writes**.

2. `modules/gaussian_splatting/renderer/render_streaming_orchestrator.h:81` —
   `String streaming_bootstrap_last_error = "none";`
   — evidence: `grep "streaming_bootstrap_last_error" -> 7 hits` (1 decl, 6 writes at
   `render_streaming_orchestrator.cpp:800, 808, 817, 823, 863, 875`, 0 reads).
   — recommendation: either plumb into diagnostics (similar to the `static_layout_fallback_last_*`
   family that *is* read) or **DELETE**.

3. `modules/gaussian_splatting/renderer/gpu_buffer_manager.h:112` —
   `bool BufferSet::debug_gpu_reading = false;` — evidence: 5 writes at
   `gpu_buffer_manager.cpp:86, 153, 248, 371, 503, 505`, 0 reads, 0 branching on the value. The
   matching `debug_cpu_writing` flag at `:111` *is* read at `gpu_buffer_manager.cpp:489` —
   `debug_gpu_reading` has no reader.
   — recommendation: **DELETE the field** (keep `debug_cpu_writing`).

4. *(No fourth pure write-only field identified with high confidence.)* A candidate —
   `StreamingStats::reused_ready_buffers` — turned out to be read inside its own log
   statements (`gpu_memory_stream.cpp:328, 331, 904` feed the counter back into `vformat`),
   so it's diagnostic-only but not write-only. Flagging here for visibility, not for deletion.

## E. Constant fields (never written post-init)

1. `modules/gaussian_splatting/renderer/gpu_memory_stream.h:29` —
   `uint32_t StreamingStats::defrag_count = 0`
   — evidence: `grep "defrag_count" -> 8 hits` — 1 decl, 0 increments/writes, but
   **read** in 3 places (`gpu_memory_stream.cpp:910` log, `core/performance_monitors.cpp:1408`
   — exposed as a live custom performance monitor `gaussian_splatting/memory_stream_defrag_count`,
   `tests/test_gpu_integration.cpp:246`).
   — diagnosis: `MemoryPool::defragment()` (gpu_memory_stream.cpp:1005) is a stub that never
   touches `stats`, and `defragment_if_needed()` does not increment the counter either.
   The counter is **always 0 at runtime**, and the editor's Debugger shows a useless row.
   — recommendation: **WIRE-IT-UP** (`stats.defrag_count++;` inside `MemoryPool::defragment` or
   the caller that invokes it) OR delete the field + its 3 read sites + the performance monitor
   entry at `performance_monitors.cpp:412`.

## F. Unused enum cases

- **Seed was incorrect.** `InvariantViolationReason` in
  `renderer/instance_pipeline_contract.h` — all 30+ case values *are* both produced (by the
  reason-returning `classify_*` functions at lines 260-430) AND consumed: `get_violation_class`,
  `get_violation_reason_name`, and the names surface into diagnostics at
  `render_streaming_orchestrator.cpp:1469, 2003`, `tile_renderer.cpp:366`,
  `resident_instance_contract_publisher.cpp:651`.
  Not dead under this lens.

No other enums survived the filter with a confirmed dead case. See "Blind spots" for scope.

## G. Unused types

1. `modules/gaussian_splatting/renderer/gpu_buffer_manager.h:24` — `struct GPUBufferManager::SortKey`
   — evidence: `grep "GPUBufferManager::SortKey" -> 0 hits`, bare `SortKey` elsewhere in module
   refers to distinct unrelated types (e.g. in `gpu_sorter.cpp`). The nested struct has no
   references.
   — recommendation: **DELETE**.

2. `modules/gaussian_splatting/interfaces/sync_policy.h:130` — class `CoarseSyncPolicy`
   (see §A item 11). Included here because the *type*, not just the methods, is dead in
   production.

## H. GDCLASS types not GDREGISTER_CLASS'd

Registered at `register_types.cpp:71-143` (32 classes). Comparing against the full GDCLASS
inventory (58 matches), the unregistered set is:

**Production (compiled unconditionally):**
- `StreamingPipeline` — `renderer/gpu_memory_stream.h:230` (dead — see §A item 12)
- `TileRenderer` — `renderer/tile_renderer.h:33` (used via `Ref<>` in production — see reviewer note)
- `PainterlyRenderer` — `interfaces/painterly_renderer.h:17` (used via `Ref<>` in production)
- `TileRasterizer` — `interfaces/tile_rasterizer.h:11`
- `RenderDeviceManager` — `interfaces/render_device_manager.h:51`
- `CoarseSyncPolicy` — `interfaces/sync_policy.h:131` (dead — see §A item 11)
- `OverflowAutoTuner` — `interfaces/overflow_auto_tuner.h:76`
- `OutputCompositor` — `interfaces/output_compositor.h:23`
- `InteractiveStateManager` — `interfaces/interactive_state_manager.h:15`
- `GPUSortingPipeline` — `interfaces/gpu_sorting_pipeline.h:16`
- `GPUCuller` — `interfaces/gpu_culler.h:35`
- `DebugOverlaySystem` — `interfaces/debug_overlay_system.h:71`
- `PainterlyMaterialManager` — `interfaces/painterly_material_manager.h:55`
- `BatchedAsyncReadback` — `renderer/batched_async_readback.h:13`
- `GaussianSplattingPerformanceMonitors` — `core/performance_monitors.h:37` — listed for
  completeness only; registered as a *singleton* via `create_singleton()` at
  `register_types.cpp:121`, so `_bind_methods()` fires when the singleton is added. Not dead.

**Editor-only (registered via `EditorPlugins::add_by_type` or resource-importer registration):**
- `GaussianEditorPlugin`, `GaussianImportDialog`, `GaussianImportSettingsDialog`,
  `GaussianImportSettingsData`, `GaussianThumbnailGenerator`, `GaussianAssetPreviewControl`,
  `GaussianDataInspectorPlugin`, `GaussianRendererInspectorPlugin`,
  `GaussianAssetInspectorPlugin`, `GaussianSplatNodeInspectorPlugin`,
  `GaussianEditorIntegration`, `GaussianSplatGizmoPlugin`, `GaussianSplatAssetPreviewGenerator`.
  — All are fine by Godot convention (EditorPlugin auto-registers its GDCLASS children via
  Plugin lifecycle); *but* if any of them uses `ClassDB::bind_method` for non-EditorPlugin
  purposes (e.g. emitting signals consumed by GDScript), those bindings still leak.

**Tests (excluded):** `MemoryValidator`, `PerformanceBenchmark`, `TestGPUIntegration`,
`TileRendererRegressionTest`, `VisualValidation`, `PainterlyChangedCounter`.

## I. Bind-method orphans / unreachable properties

For each production class above that (a) has a non-empty `_bind_methods()` AND
(b) is not `GDREGISTER_CLASS`'d, every `ClassDB::bind_method` call in that body is an orphan
because ClassDB has no entry for the class. Cross-ref with §H:

1. `OverflowAutoTuner::_bind_methods()` — `overflow_auto_tuner.cpp:31-36` — 5 bindings, 0 reachable.
2. `OutputCompositor::_bind_methods()` — `output_compositor.cpp:99` (body present).
3. `InteractiveStateManager::_bind_methods()` — `interactive_state_manager.cpp:33`.
4. `GPUSortingPipeline::_bind_methods()` — `gpu_sorting_pipeline.cpp:349`.
5. `GPUCuller::_bind_methods()` — `gpu_culler.cpp:82`.
6. `DebugOverlaySystem::_bind_methods()` — `debug_overlay_system.cpp:176`.
7. `PainterlyMaterialManager::_bind_methods()` — `painterly_material_manager.cpp:10`.
8. `PainterlyRenderer::_bind_methods()` — `painterly_renderer.cpp:53`.
9. `RenderDeviceManager::_bind_methods()` — `render_device_manager.cpp:80`.
10. `TileRasterizer::_bind_methods()` — `tile_rasterizer.cpp:140`.

(`TileRenderer` — not audited line-level, but same pattern; confirm before cleanup.)

Each of these `_bind_methods()` bodies is either (a) dead code that can be deleted, or
(b) a sign that the class should actually be `GDREGISTER_CLASS`'d (if GDScript/editor access is
wanted). The current state — GDCLASS + `_bind_methods` + no GDREGISTER — is the worst of
both worlds: compile cost with zero reflection reachability.

## Top 10 safest deletions

Ordered: no transitive callers, no tests that compile the symbol's body, high confidence.

1. `HierarchicalSplatStructure::calculate_importance` (lod/hierarchical_splat_structure.cpp:492) — 26 LOC.
2. `RadixSort::sort_async_with_timeline` (renderer/gpu_sorter.cpp:2391 + .h:363) — 8 LOC.
3. `GPUBufferManager::SortKey` nested struct (renderer/gpu_buffer_manager.h:24) — 4 LOC.
4. `CoarseSyncPolicy` full class (interfaces/sync_policy.h:130-146) — 17 LOC + tests still
   compile because `test_integration.cpp` is the only consumer and is test-gated.
5. `GaussianSplatRenderer::get_cached_streaming_route_policy` + `_source` (renderer/gaussian_splat_renderer.cpp:1343, 1348; .h:792-793) — 16 LOC.
6. `mutable bool octree_dirty` field + 5 write sites (core/gaussian_data.h:346; gaussian_data.cpp:168, 261, 362, 785; gaussian_data_edits.cpp:220).
7. `BufferSet::debug_gpu_reading` field + 5 writes (renderer/gpu_buffer_manager.h:112; gpu_buffer_manager.cpp:86, 153, 248, 371, 503, 505).
8. `GaussianImportDialog::_schedule_preview_update / _on_preview_debounce_timeout / _process_queued_preview`
   header declarations (editor/gaussian_import_dialog.h:151-153) — 3 lines.
9. `GaussianMemoryStream::_wait_for_upload / update_visible_range / set_lod_ranges / update_culling_mask`
   header declarations (renderer/gpu_memory_stream.h:146, 197, 202, 203) — 4 lines. Also
   remove `get_sort_keys_buffer` decl + its 2 callers in performance_benchmark.cpp:375, 398
   (alternative: define the method).
10. `PainterlyPassGraph::get_passes()` + `find_pass()` + private `passes` field +
    `_rebuild_passes()` + `struct PassNode` / `enum PassId` / `enum PassType`
    (renderer/painterly_pass_graph.h:20-38, 62, 66, 90-91; .cpp:126, 310). ~80 LOC bundle
    — highest payoff per byte deleted, touches only the PassGraph files. Precondition:
    confirm no external fork of the plugin consumes these.

## Things that LOOK dead but aren't (reviewer false-positive guards)

The following appeared in the seed list for this audit and I verified each one. **Do not remove**
them citing prior findings:

- `GPUBufferManager::DeferredDeletionQueue` nested class — **instantiated**. Held as
  `GPUBufferManager::DeferredDeletionQueue deletion_queue;` at
  `renderer/render_types/render_facade_state_types.h:50`, used at
  `renderer/gaussian_splat_renderer.cpp:1177 (flush_all)`, `:2088 (process_frame)`,
  `renderer/render_sorting_orchestrator.cpp:205 (queue_free)`.
- `core/gaussian_streaming.cpp` `streaming_bootstrap_last_error` — seed attributed this to the
  wrong file. The field actually lives on `RenderStreamingOrchestrator` in
  `renderer/render_streaming_orchestrator.h:81`. It IS write-only (6 writes) — captured under §D.
- `renderer/render_streaming_orchestrator.cpp::device_orchestrator` — the seed claims "captured
  in ctor, never dereferenced". In fact it's dereferenced transitively through dozens of
  delegations on `render_device_orchestrator.cpp:470-544` which call `device_orchestrator->...`
  every time. Not dead.
- `tests/memory_validator.cpp` (1,374 LOC) has zero `TEST_CASE` macros AND a header
  `memory_validator.h` referenced by `tests/test_memory_leak_detection.h` + `tests/test_macros.h`.
  **Low-confidence dead**: it's compiled under `tests=yes`, but the symbols are only reached if a
  `TEST_CASE` instantiates `MemoryValidator`. Needs an integration-scope search.
- `tests/performance_benchmark.cpp` (804 LOC) — same shape. Also the call-sites of the
  undefined `GaussianMemoryStream::get_sort_keys_buffer` (§B item 2). These two files
  together are probably dead but need a broader integration-suite check before deletion.
- `SCsub:48 globs only tests/*.h` — **seed was incorrect**. Actual glob at
  `modules/gaussian_splatting/SCsub:227` is `"tests/*.cpp"`. All 29 `tests/*.cpp` files are
  compiled under `tests=yes`, so their TEST_CASE bodies are reachable — not dead. (But the two
  above still have zero TEST_CASE macros, which is a different issue.)
- `InvariantViolationReason` enum — **30+ cases are alive**, reasons surface through
  `get_violation_reason_name` into diagnostics (§F). Seed over-stated the staleness.
- `_forget_tile_renderer_outputs`, `_warn_tile_depth_copy_incompatible`,
  `_release_shared_dynamic_asset`, `_initialize_on_render_thread`,
  `_teardown_on_render_thread`, `_dispatch_call_on_render_thread_blocking`,
  `_notify_render_thread_dispatch_completed` — all reached via
  `callable_mp(this, &GaussianSplatRenderer::method)` and/or direct call from orchestrator
  files. Not dead.

## Blind spots

- **Shaders**: dead GLSL entry-points, stale `#ifdef` branches, and unused uniform blocks are
  NOT scanned. A separate shader-lens audit is warranted.
- **Python / SCons auxiliary scripts** under `tests/` and `shaders/` were not scanned.
- **`editor/*.cpp`** was scanned at the class-registration level (§H) and for the three
  debounce methods (§B 6-8), but I did not enumerate private helpers per editor file.
- **Template specialisations** of `Hash<T>`, `Comparator<T>` etc. were not traversed.
- **GDScript / `doc_classes/*.xml`** cross-reference: any property declared in XML but not
  bound, or vice versa, was NOT checked. Known to drift in Godot modules.
- **Cross-TU inline methods** compiled into many objects (e.g. header-only `_FORCE_INLINE_`
  helpers): if every call-site were removed they'd still appear as "one hit" in my grep because
  the definition counts. I under-counted in this direction; low-confidence items are flagged.
- **`Ref<T>` held only in test fixtures** — I intentionally let test-only consumers disqualify a
  symbol from "dead" only when the test file was clearly production-guarded. There may be
  genuinely-test-only classes I conservatively left under §H. Tolerating that is better than
  tearing out scaffolding the author wants.

Verified spot-checks before commit:
- §A.2 `get_passes` — re-grepped `pass_graph.(get_passes|find_pass)|graph->(get_passes|find_pass)` → 0.
- §B.2 `get_sort_keys_buffer` — re-opened `performance_benchmark.cpp` to confirm the two call sites.
- §D.1 `octree_dirty` — re-grepped for `if *\( *octree_dirty` / `octree_dirty *\)` → only 1 decl.
- §E.1 `defrag_count` — confirmed 0 increments anywhere in `gpu_memory_stream.cpp`.
- §H registry list — diffed every `GDCLASS(Foo, ...)` hit against every `GDREGISTER_*(Foo)` hit
  on `register_types.cpp`.

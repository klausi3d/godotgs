# Duplications — Deep Sweep

## Summary

The module carries heavy *exact* and *structurally-parallel* duplication. The densest cluster is the renderer orchestrators: the same fingerprint/hash scaffold, the same "publish identical fields to N dictionaries" pattern, the same identity-sort-buffer write sequence, and the same per-file `_get_*_setting` wrapper trio are re-implemented across many files. The IO layer hosts a ~90%-identical PLY⇄SPZ fork that is already drifting. Two `LayoutHintFailureReason` enums with overlapping-but-different values live on either side of the streaming/orchestrator boundary — a latent telemetry-correctness hazard, not just cleanliness. Six config classes (`Float16Config`, `QuantizationConfig`, `SHConfig`, `SortingConfig`, `GpuSortingConfig`, `PipelineFeatureSet`) share the same five-method boilerplate. A realistic consolidation could cut ~2,500 LOC with no semantic change.

## Method

Grep for candidate patterns — function-name prefixes (`_get_*_setting`, `_apply_*_diagnostics`, `_mix_*_generation`, `copy_local_vector`, `_merge_dictionary`), enum stems (`LayoutHintFailureReason`, `StreamingReadinessState`), repeated project-settings path strings, repeated `buffer_update(sort_indices_buffer, …)` calls, duplicate `NOTIFICATION_ENTER_*` bodies — then Read to confirm line ranges and look for nearby drifted copies. Every seeded location was verified and the list was expanded by sweeping the 10 renderer orchestrators and the ~20-file `core/streaming_*` cluster for parallel APIs. Spot-checks against 5 cited locations (PLY vs SPZ at L25–132; sort-index buffer writes at L998/L1052/L1212; visibility branch at L374–490; `copy_local_vector` at L44 and L28; `_get_*_setting` wrappers at L17–27 in `sorting_config.cpp`) all matched.

## Duplications catalog

### 1. PLY vs SPZ resource importers — 90% copy-paste, already drifting

- **Locations:** `modules/gaussian_splatting/io/resource_importer_ply.cpp:1-608` vs `modules/gaussian_splatting/io/resource_importer_spz.cpp:1-441`
- **Nature:** drifted. Shared skeleton; PLY has `OPTION_VALIDATE`, `OPTION_CUSTOMIZED`, and legacy-key fallbacks in `_get_bool_option`/`_get_int_option` that SPZ dropped; PLY writes `.gsplatcache` to `r_gen_files`, SPZ does not; PLY metadata sets `dc_encoding = "legacy_bias"` and adds `quality_customized`, SPZ uses `"linear_rgb"`.
- **Size:** ~950 LOC total; shared skeleton ~300 LOC (importer registration, `_get_*_option`, `_compute_final_splat_count`, `_merge_gaussian_range`, `_build_compression_flags`; extraction loop L343–369 == L308–334; option-list builder L170–226 == L150–202; option-dict builder L395–415).
- **Why duplicated:** SPZ was forked from PLY and maintained separately; compression handling, legacy-key support, and metadata conventions have drifted.
- **Recommendation:** CONSOLIDATE via `GaussianImporterBase` (header-only or CRTP) owning the shared option helpers, option-list build, extraction loop, and save pipeline. Subclasses implement `load_file`, `get_recognized_extensions`, and append format-specific metadata.
- **Effort to consolidate:** day

### 2. Three fingerprint functions in render_streaming_orchestrator — structurally parallel

- **Locations:** `modules/gaussian_splatting/renderer/render_streaming_orchestrator.cpp:46-79` (`_compute_instance_pipeline_resource_fingerprint`), `:81-104` (`_compute_instance_asset_remap_fingerprint`), `:106-115` (`_compute_instance_pipeline_upload_fingerprint`)
- **Nature:** structurally-parallel. All three seed a magic prime, call `_mix_rid_generation` / `_mix_u32_generation` / `_mix_content_generation` N times, and return. The three seeds (`0x6a09e667f3bcc909`, `0x510e527fade682d1`, `0xbb67ae8584caa73b`) differ only by which SHA-256 IV constant was picked.
- **Size:** ~70 LOC across all three.
- **Why duplicated:** each fingerprint was added in isolation using the same pattern.
- **Recommendation:** CONSOLIDATE into a `Fingerprint64` builder with fluent `.mix_rid(...)` / `.mix_u32(...)` calls; each site picks its own seed. Removes ~40 LOC of scaffold while keeping call sites self-documenting.
- **Effort to consolidate:** hour

### 3. Three identical sort-index-buffer write blocks

- **Locations:** `modules/gaussian_splatting/renderer/render_sorting_orchestrator.cpp:998-1018` (identity fallback), `:1052-1069` (bootstrap cull-order path), `:1212-1218` (CPU-sort path using the helper), helper `apply_sort_buffer_update` at `:1092-1111`
- **Nature:** drifted. Same 6-step sequence — resize `sort_index_bytes`, fill with indices, bind host context, `ensure_sort_buffers`, resolve resource owner, `buffer_update`. Only the CPU-sort path (L1212) uses the helper; the other two inline the logic.
- **Size:** ~90 LOC across three copies; helper is ~20 LOC.
- **Why duplicated:** helper was extracted but the rollout stopped; identity-fallback and bootstrap paths predate it.
- **Recommendation:** CONSOLIDATE — replace the two inline copies with `apply_sort_buffer_update(count)`. Removes the two `target_device` owner-resolution blocks, the most drift-prone piece.
- **Effort to consolidate:** hour

### 4. Grid vs non-grid branches of `update_chunk_visibility`

- **Locations:** `modules/gaussian_splatting/core/streaming_visibility_controller.cpp:374-452` (grid branch) vs `:453-490` (fallback linear branch)
- **Nature:** drifted. Both perform identical per-chunk work (distance, padded-bounds build with `chunk_radius_multiplier` + `chunk_frustum_padding`, frustum test, visibility/stats update). The grid branch adds a `visited[]` filter and runs its loaded/resident-stats accumulation in a separate pre-pass at L402–410. The two copies will silently diverge as padding math evolves.
- **Size:** ~85 LOC across the two bodies.
- **Why duplicated:** spatial-grid acceleration was added as a new branch; the per-chunk body was inlined rather than factored out.
- **Recommendation:** CONSOLIDATE — extract `_classify_chunk_visibility(system, i, camera_pos, frustum_planes)` for the padded-bounds + frustum + stats work and call from both branches. Grid branch keeps its pre-pass + `visited[]` + O(candidates) loop; linear branch loops over all chunks.
- **Effort to consolidate:** hour

### 5. Float16Utils vs QuantizationConfig — two overlapping compression subsystems

- **Locations:** `modules/gaussian_splatting/renderer/float16_utils.cpp:1-314` + `float16_utils.h`, `modules/gaussian_splatting/renderer/float16_config.cpp:1-80` + `float16_config.h`, `modules/gaussian_splatting/renderer/quantization_config.cpp:1-80` + `quantization_config.h`
- **Nature:** structurally-parallel. Both `Float16Config` and `QuantizationConfig` define a global instance plus `load_from_project_settings` / `save_to_project_settings` / `reset_to_defaults` / `validate` / `print_config_summary`; they live under `rendering/gaussian_splatting/data/` vs `…/compression/` and control overlapping knobs (`float16_positions` vs `quantize_positions`; `enable_position_quantization` vs `per_chunk_quantization`). `Float16Utils` (314 LOC) owns the math; `QuantizationConfig` duplicates a subset of the policy surface without calling into it.
- **Size:** ~725 LOC combined; the config scaffolding alone is ~160 LOC that should be one class.
- **Why duplicated:** float16 came first; per-chunk quantization was bolted on as a separate subsystem instead of extending the existing config.
- **Recommendation:** CONSOLIDATE — merge the two configs into `GaussianStorageConfig` with nested `float16` and `quantization` sub-structs. Keep `Float16Utils` as a math library; route runtime decisions through the merged config.
- **Effort to consolidate:** day

### 6. `copy_local_vector` helper — exact duplicate across TUs

- **Locations:** `modules/gaussian_splatting/core/gaussian_data.cpp:42-51` vs `modules/gaussian_splatting/core/gaussian_data_io.cpp:25-36`
- **Nature:** exact. Both in anonymous namespace, same template signature; the only delta is a `uint32_t` vs `int` loop counter.
- **Size:** 20 LOC.
- **Why duplicated:** `gaussian_data.cpp` was split into topical TUs (`_io.cpp`, `_gpu.cpp`, `_edits.cpp`, `_color_grading.cpp`, `_animation.cpp`, `_octree.cpp`); each copied the helper because it was in an anonymous namespace.
- **Recommendation:** CONSOLIDATE — move the template into `core/gaussian_data.h` (or a new `core/gaussian_data_utils.h`) and delete both copies. Trivially safe.
- **Effort to consolidate:** hour

### 7. Wind + effector project-settings read block

- **Locations:** `modules/gaussian_splatting/renderer/render_pipeline_stages.cpp:1678-1726`
- **Nature:** drifted. 49 LOC of `static const StringName …_path("rendering/gaussian_splatting/…")` declarations followed by 23 parallel `_get_float_setting(ps, …_path, default)` calls. Wind has 7 fields, sphere effector 7, lighting 6 — all read via the same triplet.
- **Size:** 49 LOC in one chunk; the "declare path, read float" triple recurs ad-hoc elsewhere in the file.
- **Why duplicated:** no schema-driven settings reader; every new field adds three lines (path declare, read call, optional clamp).
- **Recommendation:** CONSOLIDATE — drive from a static `{path, dest_ptr, default, min, max}` table walked once. Alternative: three per-category helpers (`_read_lighting_settings`, `_read_wind_settings`, `_read_effector_settings`).
- **Effort to consolidate:** hour

### 8. Two parallel `LayoutHintFailureReason` enums — *latent correctness hazard*

- **Locations:** `modules/gaussian_splatting/renderer/render_streaming_orchestrator.cpp:171-181` (9 values) vs `modules/gaussian_splatting/core/gaussian_streaming.cpp:84-…` (~17 values)
- **Nature:** structurally-parallel but semantically different. Both enums share 7 values (`HINTS_EMPTY`, `HINT_COUNT_ZERO`, `HINT_NON_CONTIGUOUS_COVERAGE`, `HINT_OVERLAPPING_RANGES`, `REMAP_SOURCE_COUNT_MISMATCH`, `REMAP_TOTAL_COUNT_MISMATCH`, `REMAP_SOURCE_INDEX_OUT_OF_RANGE`). The `gaussian_streaming` copy adds 10 more (`DATA_NULL`, `SPLAT_COUNT_ZERO`, `HINT_START_OUT_OF_RANGE`, `HINT_RANGE_OUT_OF_RANGE`, `HINT_CHUNK_COUNT_OVERFLOW`, `REMAP_FLAG_REQUIRED`, `REMAP_FLAG_UNEXPECTED`, `REMAP_HINT_CHUNK_TOO_LARGE`, `REMAP_OFFSET_OUT_OF_RANGE`, `REMAP_SOURCE_INDEX_DUPLICATE`). Each has its own `_layout_hint_reason_code(...)` and `_layout_hint_reason_category(...)`. Reason codes published to telemetry are NOT interchangeable — consumers parsing `layout_hint_last_reason` see different string spaces depending on the producing layer.
- **Size:** ~120 LOC total (two enums + two stringifiers + two category mappers).
- **Why duplicated:** the orchestrator was added later as a check layer; rather than `#include`ing the streaming enum, a trimmed copy was declared locally.
- **Recommendation:** CONSOLIDATE — one `LayoutHintFailureReason` in `core/gaussian_streaming.h` (or a shared `streaming_contract_types.h`), one stringifier. The orchestrator uses a subset by convention. Fixes a real telemetry inconsistency, not just cleanliness.
- **Effort to consolidate:** day

### 9. 3× duplicated layout-hint metrics publication

- **Locations:** `modules/gaussian_splatting/renderer/render_streaming_orchestrator.cpp:276-344` (`_apply_layout_hint_orchestrator_diagnostics`). The same 10 fields (`fallback_total`, `fallback_io_total`, `fallback_primary_total`, `last_usage`, `last_reason`, `last_reason_category`, `last_context`, `last_detail`, `reason_counts`, `category_counts`) are written to the `orchestrator_validation` Dictionary at `:295-305`, then to `layout_hint_validation` with `orchestrator_`-prefixed keys at `:308-324`, then flat into `r_streaming_metrics` at `:327-336`.
- **Nature:** exact — same value into three destinations with three naming schemes.
- **Size:** ~40 LOC of triplicated writes.
- **Why duplicated:** downstream consumers (HUD, telemetry JSON, test harness) each expect a different key schema; rather than unify, all three are published.
- **Recommendation:** CONSOLIDATE — canonical nested Dictionary (`layout_hint_validation.orchestrator.*`); update the 2–3 consumers and delete the other writes. Cheapest intermediate fix: extract `_publish_layout_hint_fields(dest, prefix, ...)` and call three times.
- **Effort to consolidate:** hour (helper extraction) or day (full schema consolidation)

### 10. `_notification_enter_tree` vs `_notification_enter_world` in GaussianSplatNode3D

- **Locations:** `modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp:319-348` (`_notification_enter_tree`) vs `:350-358` (`_notification_enter_world`)
- **Nature:** drifted. Both call `_ensure_renderer`, `_update_render_instance`, `_register_shared_renderer`, `_update_debug_hud_visibility`. `enter_tree` additionally registers with the manager, optionally auto-loads, updates bounds/visibility, finds the editor viewport, and updates the render-target cache. `enter_world` additionally replays pending color grading and reapplies debug settings.
- **Size:** ~40 LOC; the 4-call renderer-setup sequence is duplicated.
- **Why duplicated:** Godot fires both notifications during attach; the author copied the renderer-ensure sequence to both to guarantee it runs whichever fires first.
- **Recommendation:** CONSOLIDATE — extract `_ensure_renderer_attached_state()` with the 4-call sequence + HUD visibility, call from both. Keep tree/world-specific work in the respective handlers.
- **Effort to consolidate:** hour

### 11. `_get_*_setting` wrapper scaffolding duplicated across 9 TUs

- **Locations:**
    - `modules/gaussian_splatting/core/gaussian_splat_manager.cpp:31-39`
    - `modules/gaussian_splatting/interfaces/gpu_culler.cpp:59-69`
    - `modules/gaussian_splatting/interfaces/gpu_sorting_pipeline.cpp:74-81`
    - `modules/gaussian_splatting/interfaces/painterly_renderer.cpp:30-…`
    - `modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp:95-103`
    - `modules/gaussian_splatting/renderer/render_debug_state_orchestrator.cpp:19-31`
    - `modules/gaussian_splatting/renderer/render_diagnostics_orchestrator.cpp:76-82`
    - `modules/gaussian_splatting/renderer/render_pipeline_stages.cpp:121-131`
    - `modules/gaussian_splatting/renderer/sorting_config.cpp:17-27`
- **Nature:** exact. Every file declares `static bool _get_bool_setting`, `static float _get_float_setting`, `static int _get_int_setting` that *just forward* to `gs::settings::get_*`. The central helpers already exist in `core/gs_project_settings.h` (L33, L82, L104).
- **Size:** 9 files × 3 funcs × ~4 LOC ≈ 108 LOC of pure forwarding.
- **Why duplicated:** these files predate `gs_project_settings.h`; they were migrated to delegate but the wrappers were never deleted.
- **Recommendation:** CONSOLIDATE — delete all 9 wrapper trios and retarget call sites to `gs::settings::get_*` (mechanical find-replace across ~177 call sites).
- **Effort to consolidate:** hour

### 12. Six parallel config classes with identical 5-method API

- **Locations:** `renderer/float16_config.h:55-68`, `renderer/quantization_config.h:60-80`, `renderer/sh_config.h:53-67`, `renderer/gpu_sorting_config.h:65-118`, `renderer/pipeline_feature_set.h:37-51`, `renderer/sorting_config.h:36`
- **Nature:** structurally-parallel. Each class has its own `load_from_project_settings()`, `save_to_project_settings() const`, `reset_to_defaults()`, `validate() const`, `print_config_summary() const`, a `SECTION_PATH` string, per-field `…_PATH` StringNames, and a global instance. Each `.cpp` is ~80–130 LOC of the same body shape.
- **Size:** ~600 LOC of parallel boilerplate (data fields are module-specific; boilerplate is not).
- **Why duplicated:** each subsystem added its own config class; no shared template.
- **Recommendation:** CONSOLIDATE — `ProjectSettingsConfigBase<Derived>` CRTP exposing `load()/save()/reset()/validate()/print()` driven by a `Derived::describe_fields()` field table. Alternative: a single `ConfigBinder` helper called from each config's `load_from_project_settings()`.
- **Effort to consolidate:** day (per class conversion, ×6)

### 13. `apply_queue_pressure_analytics` vs `apply_queue_pressure_diagnostics` — two methods, same body

- **Locations:** `modules/gaussian_splatting/core/streaming_telemetry_adapter.cpp:20-28`
- **Nature:** exact. Both methods forward to the anonymous-namespace `_apply_queue_pressure_common`; the API split only exists so consumers can insert the same snapshot into two differently-named outer dictionaries.
- **Size:** ~30 LOC file total.
- **Why duplicated:** parallel metric publishers for "analytics" vs "diagnostics" dictionaries — same pattern as entry #9.
- **Recommendation:** CONSOLIDATE — expose one `apply_queue_pressure(Dictionary &dest, const Snapshot &s)`; callers name their own dictionary.
- **Effort to consolidate:** hour

### 14. MemoryValidator + PerformanceBenchmark — test harness, not production

- **Locations:** `modules/gaussian_splatting/tests/memory_validator.cpp:1-1374`, `modules/gaussian_splatting/tests/memory_validator.h`, `modules/gaussian_splatting/tests/performance_benchmark.cpp:1-804`, `modules/gaussian_splatting/tests/performance_benchmark.h`
- **Nature:** *not* duplication of production logic. Verified by grep: both classes are referenced only from `tests/test_memory_leak_detection.h` and similar test-only headers. They duplicate *concepts* that also live in production (memory tracking; perf counters in `core/performance_monitors.*` and `renderer/gpu_performance_monitor.*`), but no code is shared.
- **Size:** 2,178 LOC, test-only.
- **Why duplicated:** separately-authored measurement framework to drive test scenarios; parallel to but not sharing code with the production monitors.
- **Recommendation:** KEEP-TWO-WITH-COMMENT — they serve the test harness and don't burden production.
- **Effort to consolidate:** week (if actually merging with production monitors — not recommended)

### 15. PLY/SPZ option PropertyInfo enum strings repeated verbatim

- **Locations:** `modules/gaussian_splatting/core/gaussian_splat_asset.cpp:113` (`"Static,Dynamic"`), `modules/gaussian_splatting/editor/gaussian_import_settings_dialog.cpp:72`, `modules/gaussian_splatting/io/resource_importer_ply.cpp:182, :216`, `modules/gaussian_splatting/io/resource_importer_spz.cpp:162, :192`
- **Nature:** exact — 4× `"Static,Dynamic"`, 2× `"Color,Density,Normals,Heatmap"`, 2× `"mobile,desktop,high,ultra,development,custom"`.
- **Size:** small, but each is a drift trap — if an enum grows, N files must change in lockstep.
- **Why duplicated:** Godot's `PROPERTY_HINT_ENUM` takes a literal string; no enum-to-hint codegen exists.
- **Recommendation:** CONSOLIDATE — one `constexpr const char *` per enum kind in `core/gaussian_splat_asset.h` or `io/gaussian_import_preset.h`; reference everywhere.
- **Effort to consolidate:** hour

### 16. `_merge_dictionary` + `Dictionary::duplicate()` pattern

- **Locations:** `modules/gaussian_splatting/renderer/render_diagnostics_orchestrator.cpp:156-162` (`_merge_dictionary`), `:950` (one call), `:1295` (one `.duplicate()` call)
- **Nature:** structurally-parallel in concept ("copy-then-extend"), but only one `_merge_dictionary` helper actually exists — the pattern is narrower than the seed suggested.
- **Size:** ~10 LOC.
- **Why duplicated:** one-off helper; not actually repeated in this module.
- **Recommendation:** LEAVE-ALONE-BECAUSE-X — the seed finding was wider than reality. Helper is fine where it is.
- **Effort to consolidate:** (n/a)

### 17. `_mix_*_generation` helpers

- **Locations:** `modules/gaussian_splatting/renderer/render_streaming_orchestrator.cpp:32-44` — `_mix_content_generation`, `_mix_u32_generation`, `_mix_rid_generation`
- **Nature:** these three are used *only* by the three fingerprint functions in entry #2; combined with #2 if the recommendation there is taken.
- **Size:** 15 LOC
- **Why duplicated:** scoped to one TU; not duplicated elsewhere.
- **Recommendation:** CONSOLIDATE (with #2) — move into the shared `Fingerprint64` builder.
- **Effort to consolidate:** hour (folds into #2)

### 18. Streaming subsystem file sprawl (20+ files in `core/streaming_*`)

- **Locations:** `modules/gaussian_splatting/core/streaming_*.{cpp,h}` (20 files: `streaming_asset_types.h`, `streaming_atlas.*`, `streaming_chunk_payload_source.*`, `streaming_config_overrides.h`, `streaming_eviction_controller.*`, `streaming_global_atlas_registry.*`, `streaming_lod_policy.cpp`, `streaming_quantization.*`, `streaming_queue_pressure_controller.*`, `streaming_runtime_state.h`, `streaming_telemetry_adapter.*`, `streaming_upload_pipeline.*`, `streaming_visibility_controller.*`, `streaming_vram_regulator.*`, plus `gaussian_streaming.{cpp,h}` at the top)
- **Nature:** structurally-parallel. The controller + state-struct + types + telemetry split follows a per-subsystem convention that produces N×3 duplication: each controller owns its `load_*_config_from_project_settings()`, its `*Config` struct, and its project-settings path strings; the shape repeats 6–8 times. `gaussian_streaming.cpp` alone has `_load_zero_visible_recovery_config_from_project_settings` (L2605), `_load_streaming_tuning_config_from_project_settings` (L4339), `_update_culling_config_from_project_settings` (L4335), `_load_prefetch_config_from_project_settings` (L4844), `_reload_debug_logging_config` (L4344), `_reload_config_if_dirty` (L2538), `_refresh_quantization_dc_compatibility` (L1646), `_refresh_primary_chunk_layout_metrics` (L2172).
- **Size:** shared-pattern scaffolding is ~300–500 LOC across 8 controllers.
- **Why duplicated:** the streaming system was split into smaller controllers, but each kept its own project-settings wiring rather than pulling it up to a shared `StreamingConfigService`.
- **Recommendation:** KEEP the file layout (the architecture is reasonable); CONSOLIDATE just the `load_*_config_from_project_settings()` pattern into a table-driven loader (shares the solution with #7 and #12).
- **Effort to consolidate:** day

### 19. Six parallel set-flag patterns on `sort_route_uid` + `cull_route_uid` across orchestrators

- **Locations:** `modules/gaussian_splatting/renderer/render_sorting_orchestrator.cpp:1021, 1087, …` plus render_streaming_orchestrator.cpp fallback paths. Each site sets `debug_state.sort_route_uid = RenderRouteUID::…` + `sorting_state.last_sort_transform_valid = false` and emits a WARN log; the shape repeats 4–5 times per file.
- **Nature:** structurally-parallel — same 3-line "mark state + log" sequence with different UID + reason string.
- **Size:** ~40 LOC across the fallback paths.
- **Why duplicated:** each fallback site was hand-authored with the same three-step ritual.
- **Recommendation:** CONSOLIDATE — `_publish_fallback_route(route_uid, reason, log_fn)` helper.
- **Effort to consolidate:** hour

### 20. `allow_legacy_resident_fallback` threaded through renderer API

- **Locations:** `modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp:1392, 1965, 1984, 2018, 2056, 2073, 2075, 2246, 2321`, `gaussian_splat_renderer.h:246, 678`
- **Nature:** structurally-parallel — legacy and streaming render paths coexist; the bool is threaded through 4 signatures and 10+ sites to choose between them. Duplicated code *path* rather than duplicated code text.
- **Size:** two parallel render paths; not cleanly quantifiable.
- **Why duplicated:** architecture freeze keeps both paths alive until streaming covers all cases.
- **Recommendation:** LEAVE-ALONE-BECAUSE-X — architecture freeze per `project_architecture_freeze.md`; this is a tracked migration, not accidental duplication. Revisit when the legacy path retires.
- **Effort to consolidate:** week (not now — gated on streaming completeness)

## Structural patterns

- **Parallel "publish to N dictionaries" metric writers.** Entries #9 and #13: one snapshot inserted into 2–3 named dictionaries under different naming schemes to satisfy different consumers. Every new telemetry field pays the duplication tax. Root cause: no canonical telemetry schema — HUD, JSON export, and tests each want a different key path.
- **Parallel config classes.** Entries #5 and #12: six classes with the same five-method API (`load/save/reset/validate/print`) wrapping a ProjectSettings section. ~600 LOC of accidental boilerplate waiting for a CRTP base or a table-driven `ConfigBinder`.
- **Parallel enum + stringifier pairs.** Entry #8 is the sharpest instance. The pattern generalizes: any cross-module protocol using a reason code tends to grow a stringifier on each side. Audit `RenderRouteUID`, `StreamingReadinessState`, and `RenderFallbackReason` next.
- **Forwarding wrapper accretion.** Entry #11: introducing a central utility *after* local copies exist leaves behind trivial forwarders that never get deleted. Sweep the module for `static (bool|float|int) _get_\w+_setting(` that just calls `gs::settings::`.
- **Anonymous-namespace helpers copied across TU splits.** Entry #6 (`copy_local_vector`) is canonical: splitting a big `.cpp` into topical files copies private helpers with it. Any anonymous-namespace template in a `.cpp` is a candidate for promotion to a header.
- **"Fast path vs fallback" branches that duplicate the per-element body.** Entry #4. Watch for this pattern when LOD evaluation, eviction sweeps, or other linear scans get spatial-acceleration optimizations — extract the per-element body first.
- **Importer forks diverging after a copy.** Entry #1. A third importer (`resource_importer_gsplatworld.cpp`) exists and would compound the cost if not consolidated now.

## Top 5 highest-leverage consolidations

1. **Entry #8 — unify `LayoutHintFailureReason` enums.** ~120 LOC saved; eliminates a real telemetry-inconsistency bug where consumers see different string spaces depending on producer layer. Highest risk-reduction-per-LOC in the catalog. **Effort:** day.
2. **Entry #1 — PLY/SPZ importer base class.** ~300 LOC deleted; also absorbs the future `gsplatworld` importer. Legacy-key handling has already diverged. **Effort:** day.
3. **Entry #11 — delete the 9× `_get_*_setting` forwarder trios.** Mechanical cleanup; ~108 LOC gone, zero behavior change, zero review burden. **Effort:** hour. Do this *today*.
4. **Entry #12 — CRTP `ProjectSettingsConfigBase` for the 6 config classes.** ~300–400 LOC saved once migrated; improves ergonomics for new configs. **Effort:** day per class; batch them.
5. **Entries #2 + #3 + #17 — fingerprint/sort-buffer/mix helpers.** An hour of work cuts ~130 LOC and removes drift risk that's already visible (one sort path uses the helper; two don't). **Effort:** hour.

## Blind spots

- **Shaders (`modules/gaussian_splatting/shaders/`) were not inspected.** Parallel `.glsl` variants for different precision/tier paths are likely — the project already ships `sort_compute_gpu.glsl` alongside `sort_radix.glsl`. A dedicated shader-duplication sweep is warranted.
- **Similarity scan was pattern-based, not AST-based.** Two 80-LOC methods that differ only in a field name would slip past unless the pattern surfaced in the name/structure. An AST-diff pass would likely find more.
- **Test fixtures and mocks in `tests/` were only spot-checked at file level.** Possible duplication, but lower leverage.
- **`painterly/`, `editor/`, `asset_management/`, `doc_classes/`, `persistence/`, `lod/`, and `interfaces/`** were touched only incidentally (via cross-references from #11 and #1). A follow-up sweep in each is advised.
- **Intra-file near-duplicates in the big switch/case state machines** are under-sampled. `gaussian_streaming.cpp` (4,928 LOC), `gaussian_splat_node_3d.cpp` (2,362 LOC), and `render_streaming_orchestrator.cpp` (2,303 LOC) likely harbor additional duplicates not exhaustively enumerated here.
- **Orchestrator `Dependencies` + `runtime_ports` scaffolding** (10 orchestrators each with the same validation constructor) is a candidate structural-duplication vector. Entry #20 touches one slice; a full sweep is not in this pass.

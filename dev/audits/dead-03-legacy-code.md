# Legacy Code — Deep Sweep

## Summary

This sweep looks specifically for paths that "once mattered but don't now" — `#if 0`, `// legacy`, `// DEPRECATED`, compat shims, fall-through stubs, version-gated dead branches, and quarantined tests. The Gaussian Splatting module is **mostly free of classic legacy rot** (no version-gated forks, only one `#if 0` block, no `_old`/`_v1`/`_v2` duplicates). The legacy that does exist clusters into five tight buckets:

1. **Project-settings compat shims** — two canonical alias-with-WARN-PRINT_ONCE plumbings (`pack_opacity`, `gpu_sorting/target_sort_time_ms`) and the `asset_type` PLY-option rename. These are intentional migration layers, safe to retain.
2. **Unreachable sort kernels** — `BitonicSort` and `OneSweepSort` shaders declare `float keys[]` (32-bit) and advertise `supports_64bit_keys = false`, but the factory forces `requires_64bit_keys = true` on the instance-sort path. Both algorithms are dead in production and fall back to `RadixSort`. This is the largest live-looking-but-dead surface in the module (~800 LOC of compute-shader + host scaffolding).
3. **Fall-through stubs** — `parallel_build=true` logs `WARN_PRINT_ONCE` and calls the same sequential builder. One live instance.
4. **Stale-constant / ABI mismatch** — `kIndirectDispatchHeaderSize = 16 B` but every shader+CPU IndirectDispatchLayout uses 24 B (has `overflow_flag` + `unclamped_total`). This is not "legacy" in the literate sense — it's a bug that masquerades as legacy.
5. **Node-level serialized-scene compatibility** — `ply_file_path`, `color_variation`, `use_occlusion_culling` on `GaussianSplatNode3D` / `GaussianSplatDynamicInstance3D`. Kept for serialized scenes; not worth removing.

Total safely-deletable LOC today: **~900** (overwhelmingly Bitonic/OneSweep scaffolding + stale pack_opacity UI + legacy LOD settings that are never read). None of the deletions are urgent; the sort-kernel cleanup is the only high-leverage one.

## Method

- Grep for explicit markers: `#if 0`, `legacy`, `DEPRECATED`, `WARN_DEPRECATED_MSG`, `WARN_PRINT_ONCE.*deprecated`, `// TODO`, `// FIXME`, `// HACK`, `// kept for`, `// old`, `// new`, `// backward`, `// compat`, `// temporary`.
- Cross-reference with `Grep files_with_matches` to find real usage of flagged APIs (deleted-only-on-paper vs. truly dead).
- Spot-check version constants and `#if`/`#ifdef` gates for one-sided deadness.
- Walk the factory selection path in `gpu_sorter.cpp` to confirm which concrete sorter classes are actually instantiable.
- Verify the fall-through stub (`parallel_build`) is the current shape.

---

## Commented-out / `#if 0` blocks

- `modules/gaussian_splatting/renderer/render_debug_state_orchestrator.cpp:32-38` — `#if 0 // Disabled for perf baseline` in `_default_pipeline_trace_enabled()`. 6 LOC. The `#else` branch (`return false`) is live. Compiler never sees the `return true` branch.
  - **Verdict: yes, safe to delete.** Replace with `return false;`. The comment tells the reader what it used to do; that belongs in git log, not in the source.

That is the **only** `#if 0` in the entire module. Good hygiene.

---

## Deprecated markers

Items with an explicit `DEPRECATED`, `WARN_DEPRECATED_MSG`, or `// Legacy` / `// deprecated` comment, and what replaced them.

| File:line | Item | Replacement | Safe to delete? |
|---|---|---|---|
| `modules/gaussian_splatting/core/gaussian_data.cpp:53-65` | `GaussianData::get_rendering_device()` — warns, returns main RD | `GaussianSplatManager::get_primary_rendering_device()` | **No.** Still called from `core/gaussian_data_gpu.cpp:144, 222` as a null-device fallback. Those callers need to be converted first. |
| `modules/gaussian_splatting/renderer/gpu_sorter.h:182-188` | `virtual float get_theoretical_complexity() const` marked DEPRECATED | `get_metrics()` (`SortingMetrics`) | **Conditionally yes.** No callers outside of the overrides themselves. Pure-virtual with three overrides, all three returning magic floats. Delete the virtual + three overrides → ~12 LOC gone, zero risk. |
| `modules/gaussian_splatting/nodes/gaussian_splat_dynamic_instance_3d.cpp:157-169` + `.h:26,53-54` | `ply_file_path` property + `set_ply_file_path` (wrapped in `#ifndef DISABLED_DEPRECATED`) | `splat_asset` / `gaussian_data` | **No — needs deprecation cycle.** Property is still `ADD_PROPERTY`-bound at `.cpp:17` and read at `.cpp:280,290,293,300`. External users with serialized scenes will hit it. Keep until a formal deprecation release. |
| `modules/gaussian_splatting/nodes/gaussian_splat_node_3d.h:151,160,544,598` + `.cpp:130-132,156-158,506-516,519-526` | `color_variation`, `use_occlusion_culling` "legacy serialized compatibility only" | Properties were unbound but `_set`/`_get` still honor the old names | **No — intentional back-compat.** Load path from serialized `.tscn` depends on it. |
| `modules/gaussian_splatting/io/resource_importer_ply.cpp:295-300`, `io/resource_importer_spz.cpp:260-265` | `OPTION_PACK_OPACITY` read, WARN_PRINT_ONCE, ignored | (nothing — feature retired) | **Conditionally yes.** Setting is still exposed in the import UI (see Compat Shims section below). Safe to delete once the UI surface is gone. |
| `modules/gaussian_splatting/interfaces/gpu_sorting_pipeline.cpp:2783-2785` | Comment: "Legacy GPU depth sort shader path has been retired" | `_sort_instance_pipeline()` is the single live path | **Already deleted**, comment is fine to drop with the next touch. 0 LOC risk. |
| `modules/gaussian_splatting/renderer/gaussian_splat_renderer.h:246,853` + `.cpp:1392,1965,2018,2056,2059,2073,2321` | `allow_legacy_resident_fallback`, `_reset_legacy_streaming_data_path_state()` | Streaming-only path; legacy = non-streaming resident | **No.** Live flag: `plan.allow_legacy_resident_fallback = !plan.streaming_requested;`. The word "legacy" here names the non-streaming backend, not a dead path. Rename to `allow_resident_backend_fallback` would be clearer. |
| `modules/gaussian_splatting/interfaces/interactive_state_manager.h:65-75,89-100,133-138` + `.cpp:246+` | "Legacy API from god class — for backwards compatibility during transition" — 8 public methods | The `IInteractiveStateManager` interface methods above them | **No.** Still called from `renderer/render_config_orchestrator.cpp:57` and `renderer/render_resource_orchestrator.cpp:71`. "Transition" never finished; the post-god-class interface is not yet the single caller. |
| `modules/gaussian_splatting/interfaces/debug_overlay_system.h:167-179` | "Legacy renderer helpers (god class extraction)" — 11 methods | `DebugOverlayCommandSink` overloads above them | **Conditionally yes.** These wrap the Sink-based versions with `build_command_sink(p_renderer)` trampolines. Remove once all external callers pass the sink directly. Low-LOC win (~25 lines of trampolines), risk = medium. |
| `modules/gaussian_splatting/lod/lod_config.cpp:356-358` | `GLOBAL_DEF(LOD_CONFIG_MIN_SCREEN_SIZE_PIXELS_PATH, 1.5f)` + `BIAS_PATH` under comment "Legacy LOD settings (kept for compatibility)" | Read via string literal in `interfaces/gpu_culler.cpp:218,222` (`"rendering/gaussian_splatting/lod/min_screen_size_pixels"`, `"rendering/gaussian_splatting/lod/bias"`). | **No — mislabeled.** Settings ARE live; the comment "kept for compatibility" is misleading because the consumer in `gpu_culler.cpp` reads them every frame. Consider renaming the comment or introducing a clearer "legacy=pre-unified-LOD-config" label. The macros `LOD_CONFIG_MIN_SCREEN_SIZE_PIXELS_PATH` / `LOD_CONFIG_BIAS_PATH` themselves are unused (consumer uses string literals), but removing the `GLOBAL_DEF` calls would strip the defaults and break consumers. |
| `modules/gaussian_splatting/renderer/gpu_debug_utils.h:12-34` | `_get_bool_setting`, `_get_int_setting` — "Legacy aliases -- delegate to the canonical gs::settings helpers" | `gs::settings::get_bool/get_int` | **No.** 13 files use the aliases. Renaming is a pure cleanup that doesn't justify its diff. |
| `modules/gaussian_splatting/tests/test_asset_dependency_manager.h:1-10` | Header file reduced to a legacy-placeholder comment | `tests/test_asset_dependency_manager.cpp` | **Yes.** File is 10 lines of comment-only content. Delete the header, fix the includes. |

---

## Fall-through stubs

Methods that warn and then run the old behavior.

- `modules/gaussian_splatting/lod/hierarchical_splat_structure.cpp:92-98` + `hierarchical_splat_structure.h:85` — `params.parallel_build`:
  ```cpp
  if (params.parallel_build && total_splats > 10000) {
      WARN_PRINT_ONCE("[...] parallel_build requested, ... falling back to sequential build.");
      build_node_recursive(root.get(), splat_data, 0, total_splats, 0, params);
  } else {
      build_node_recursive(root.get(), splat_data, 0, total_splats, 0, params);
  }
  ```
  Both arms call the identical function. The `if` exists only to emit a WARN.
  **Verdict: yes, safe to delete.** Either implement parallel build or remove the `parallel_build` field from `BuildParams` along with the warning. Keeping an advertised feature that does nothing is worse than the honest simpler API.

- `modules/gaussian_splatting/core/gaussian_data_gpu.cpp:140-145` — `create_gpu_buffer()` without owner device:
  ```cpp
  if (!rd) {
      WARN_PRINT_ONCE("create_gpu_buffer() called without explicit owner device; falling back to main RenderingDevice");
      rd = get_rendering_device();  // this itself warns "deprecated"
  }
  ```
  Double-warn fall-through. Callers should never pass nullptr. **Verdict: conditionally yes** — audit the two callers, make the parameter non-null, remove the branch.

- `modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp:2059` — `WARN_PRINT_ONCE("[...] Resident instance contract not ready; falling back to legacy resident path (...)")` — not a stub, a real fallback that runs a different path. **Keep.**

No others found.

---

## Dead version branches

- `modules/gaussian_splatting/io/gaussian_splat_world_io.cpp:22,172,545` + `io/resource_importer_gsplatworld.cpp:32,58` — `kWorldVersion = 1`, `world_version = 1u`. Single-version checks, rejecting `!=` returns `ERR_FILE_CORRUPT`. No `if (version == N)` branching. Clean. No dead half.
- No `#ifdef VERSION_X` / `#ifndef VERSION_X` version-gated forks found anywhere.
- No `#ifdef FEATURE_FLAG_THAT_IS_NEVER_DEFINED` branches found.
- `GS_DEBUG_FREE_TRACKING` in `renderer/gpu_debug_utils.h:180-234`: default is `1`, and both arms are real (the `#else` strips logging for release). This is an intentional dev/ship toggle, not legacy.

**The single notable "dead arm" style fork** is more subtle — the whole `BitonicSort`/`OneSweepSort` infrastructure described in the next section: one side of the (implicit) "is instance-sort path live?" fork never executes.

### Unreachable sort kernels (biggest finding)

`modules/gaussian_splatting/renderer/gpu_sorter.cpp:922-1023` — `GPUSorterFactory::create_sorter()`.

Contract enforced by factory:
- Line 949: `const bool requires_indirect = true;`
- Line 950: `const bool requires_64bit_keys = key_config.key_bits > 32;` (every instance path passes 64-bit)
- Line 925-927: `if (key_config.key_bits != 64) { key_config.key_bits = 64; }` — forced to 64.
- Line 985-1000: if requested algorithm doesn't meet requirements, **fall back to RADIX**.

Capability reports:
- `BitonicSort::get_capabilities()` at `:3314-3322` — `supports_64bit_keys = false`, `supports_indirect = false`.
- `OneSweepSort::get_capabilities()` at `:3359-3366` — same.
- `RadixSort::get_capabilities()` at `:3333-3343` — both true.

Consequence: in the live instance-sort pipeline, **only RadixSort is ever instantiated**. Every user-visible `force_algorithm = bitonic|onesweep` setting silently selects RadixSort via the fallback at `:985-1000`.

Evidence this is legacy, not work-in-progress:
- Bitonic uses `float keys[]` (`:582, :598, :604-605`). Cannot hold 64-bit tile+depth keys.
- OneSweep shaders declare `float keys[]` at `:2681, :2749, :2907, :2911`. Same problem.
- Bandwidth calc at `:878` computes `constexpr uint32_t key_stride_bytes = 8` (64-bit) — directly contradicts the shader's float stride. Clear mid-refactor sediment.
- OneSweep does 4 submit-and-sync stalls per sort (`:3147, :3247`, once per pass × 4 radix passes). The "15 FPS cap" pattern the RadixSort path explicitly removed at `:2572-2574` is still present in OneSweep. Matches the seed finding.
- BitonicSort's `initialize` creates resources at `:565-641`. No production flow reaches it.

**Verdict on BitonicSort + OneSweepSort as a whole:**
- **Conditionally yes — safe to delete today** from the instance-sort path; ~800 LOC of host code + 4 shader strings. No external callers because the factory gates them out.
- Full deletion requires also removing:
  - `GPUSorterFactory::ALGORITHM_BITONIC` / `ALGORITHM_ONESWEEP` enum values + `force_algorithm` project-setting choices 2,3 (`core/gaussian_splat_manager.h:178`, `.cpp:283,1146`).
  - `_probe_algorithm` and `evaluate_auto_policy` arms.
  - `interfaces/gpu_sorting_pipeline.cpp:175,177` switch cases.
- Risk: if anyone re-targets instance sort to 32-bit keys in the future, they'll want these back. But right now they are **unreachable code** and the shaders are **broken for the actual key layout**. Better to delete than to leave a trap.
- This is the single largest, cleanest deletion available in the module.

---

## Compat shims & legacy-key fallbacks

### PLY/SPZ importer option migration

`modules/gaussian_splatting/io/resource_importer_ply.cpp:29,36,38,45,59-62,72-75,255-256,282,296-300`:

- `OPTION_ASSET_TYPE_LEGACY = "asset_type"` ↔ `OPTION_ASSET_TYPE = "general/asset_type"`
- `OPTION_VALIDATE_LEGACY = "validate_required_properties"` ↔ `OPTION_VALIDATE = "validation/validate_required_properties"`
- `OPTION_WARN_MISSING_LEGACY = "warn_missing_optional"` ↔ `OPTION_WARN_MISSING = "validation/warn_missing_optional"`
- `OPTION_PACK_OPACITY = "compression/pack_opacity"` — read, warned, ignored. No replacement.

Fallback happens silently in `_get_bool_option` / `_get_int_option` helpers (no deprecation WARN on the rename cases — just the pack_opacity one).

- **Verdict on the three rename shims:** **No — needs deprecation cycle.** Existing `.import` files in user projects carry these keys. Removing without a WARN → silent loss of import settings on reimport.
- **Verdict on `OPTION_PACK_OPACITY`:** See below under pack_opacity surface.

### `editor/gaussian_import_settings_dialog.cpp:465-484` — one-off `legacy_key_map`

Maps `"asset_type"` → `"general/asset_type"` when parsing `.import` sidecar files. Matches the importer shim above. **Keep in lockstep with that shim.** Safe to delete when the shim is retired.

### `persistence/gaussian_scene_serializer.cpp:908-990` — legacy_probe checksum fallback

Reads scene files that were written before strict checksums, only when `enable_checksum == false`. Not dead; live fallback path for old serialized scenes. **Keep.**

### `io/ply_loader.cpp:238-296` — `.gsplatworld` legacy cache rename

Falls back to the pre-rename cache path and migrates it in-place.
**Verdict: no.** Live migration for user assets on disk. Keep for at least one release cycle after the rename shipped.

### `renderer/sorting_settings_utils.h:17-21,56-60` — `gpu_sorting/target_sort_time_ms` → `sorting/target_sort_time_ms`

Read-only compat alias with explicit `WARN_PRINT_ONCE` mentioning "Legacy alias support is read-only compatibility and will be removed after project migration." Precedence rules are covered by the corresponding test in `tests/test_config_validation.h:106-147`.
**Verdict: no — this one has an explicit migration message promising removal later.** Track it, remove in a named release.

### `pack_opacity` — the largest compat surface

`pack_opacity` is a retired compression flag. Still referenced in:
- `editor/gaussian_import_dialog.cpp:230-235,464,528-529,591-592,684` — a CheckBox with `set_disabled(true)` and `TTR("Pack Opacity (Deprecated)")`.
- `editor/gaussian_import_dialog.h:41-59,79,110` — field + docblock.
- `editor/gaussian_import_settings_dialog.cpp:93,269,455` — property info + defaults written into inspector.
- `editor/gaussian_editor_services.cpp:269-271` — read from metadata, printed in summary.
- `editor/gaussian_thumbnail_generator.{h:59,cpp:537-544}` — `p_pack_opacity` parameter, used to inflate a memory estimate.
- `io/gaussian_import_preset.{h:18,cpp:21,40,59,78,97}` — struct field + 5 preset defaults (`mobile=true`, rest `false`).
- `io/resource_importer_{ply,spz}.cpp:295-300/260-265` — read with WARN, ignored.
- `logger/gs_debug_trace.cpp:30,201,339` — telemetry field `last_pack_opacity`.

**Verdict: conditionally yes — coordinated removal.** Every consumer path just parrots the boolean around; it doesn't actually affect packing. The CheckBox is already `disabled(true)`. Proposed clean-up: delete the field from `gaussian_import_preset`, the CheckBox, the metadata plumbing, the memory-estimate inflation, and the WARN_PRINT_ONCE warnings in the importers. Estimated deletion: ~60 LOC across 8 files. Risk: zero runtime, some inspector-layout churn.

---

## Stale TODOs & FIXMEs with concrete descriptions

Only 4 TODO/FIXME markers across all `.cpp`/`.h`:

- `renderer/gpu_sorter.h:68`, `:137`, `renderer/gpu_sorter.cpp:3126` — three variants of "TODO: Replace with GPU timestamps when RenderingDevice exposes per-dispatch query API." Blocked on engine API, not module work. **Actionable only externally.**
- `tests/test_gaussian_importer.h:597` — "TODO: set_splat_asset does not yet auto-switch to QUALITY_CUSTOM with relaxed LOD for full-fidelity assets." Test case returns early via `MESSAGE("Skipping - aspirational test, requires set_splat_asset fidelity auto-detection (not yet wired)")`. **Actionable in-module.** Either wire the auto-detection or drop the test skeleton.

---

## Test-quarantine rot

299 `MESSAGE("Skipping...")` early-returns across 14 test files. Breakdown:

| File | Skips | Character of most skips |
|---|---|---|
| `test_renderer_pipeline.h` | 139 | Headless-mode guards (no RenderingServer/RenderingDevice) — intentional, not legacy. |
| `test_gpu_streaming.cpp` | 90 | Headless guards. |
| `test_gaussian_splat_node.h` | 22 | Headless guards. |
| `test_scene_director_submission_scaffolding.h` | 16 | Headless guards. |
| `test_gaussian_streaming_lifecycle.cpp` | 9 | Headless guards. |
| `test_gpu_streaming.h` | 5 | Headless guards. |
| `test_integration.cpp` | 5 | Headless guards. |
| `test_memory_leak_detection.h` | 3 | Headless guards. |
| `test_render_validation.h` | 3 | Headless guards. |
| `test_gaussian_importer.h` | 2 | 1 aspirational TODO (see above), 1 headless guard. |
| `test_gpu_sorting.h` | 2 | Headless guards. |
| `test_phase1_integration.{cpp:1, h:1}` | 2 | Headless guards. |
| `test_macros.h` | 1 | Defines the skip macro. |

**Verdict:** the volume is misleading. Nearly every skip is a conditional short-circuit at the top of a TEST_CASE because the headless CI runner cannot instantiate a `RenderingServer`. Those skips are structural, not legacy-quarantine. The only actually-dead-quarantined test is:

- `tests/test_gaussian_importer.h:596-599` — unconditional `MESSAGE("Skipping - aspirational test, ...")` followed by `return;`. All 40+ lines of setup below the return are dead. **Verdict: yes, safe to delete** — either implement the feature it tests, or delete the case. Leaving an `return;` as the first statement in a `TEST_CASE` is rot.

- `tests/test_asset_dependency_manager.h` (10 lines) — pragma-once header with only a comment saying "Legacy placeholder. Canonical coverage lives in tests/test_asset_dependency_manager.cpp." Still `#include`d by `tests/test_gaussian_splatting.h:20`. **Verdict: yes, safe to delete** — remove both the file and the include line. The coverage is in the `.cpp`, which registers its tests independently.

The **4 pre-existing test failures on base** (`project_test_baseline.md`) live alongside these skips — they are not skips, they are real failures. Out of scope for this audit (tracked separately).

---

## Stale constants / ABI mismatch

- `renderer/pipeline_io_contracts.h:29` — `kIndirectDispatchHeaderSize = offsetof(IndirectDispatchLayout, overflow_flag) = 16 B`.
  Every shader (`tile_binning.glsl:115-116`, `tile_rasterizer_compute.glsl:110-111`, `tile_rasterizer.glsl:107-108`, `tile_prefix_scan.glsl:63-64`, `includes/tile_raster_common.glsl:32-33`) declares a 24 B layout (6 × uint32).
  `renderer/gpu_sorter.cpp:2092,2094,2104,2106` allocates the buffer at 16 B.
  `renderer/gpu_sorter.cpp:1341-1347` writes only the first 16 B (`IndirectDispatchHeader` has 4 uint32s).
  The shader writes `overflow_flag` and `unclamped_total` at offsets 16 and 20 into the same buffer.
  **Verdict: not legacy — a latent GPU-side out-of-bounds write.** Out of scope for "delete legacy" but should be flagged to the maintainer. The constant name is misleading; either rename to `kIndirectDispatchReadSize` + allocate `sizeof(IndirectDispatchLayout)` for storage, or fix the allocation sites.

---

## Top 10 safest deletions (ranked by LOC × low risk)

1. **Delete `BitonicSort` + `OneSweepSort` from the factory path.** ~800 LOC. All unreachable via `requires_64bit_keys` gate. Zero runtime risk because fallback is already `RadixSort`. Biggest single sweep available.
2. **Delete the `get_theoretical_complexity()` virtual + 3 overrides.** ~12 LOC. No callers. Explicitly DEPRECATED-comment on the declaration. Instant zero-risk win.
3. **Delete the two unused macros `LOD_CONFIG_MIN_SCREEN_SIZE_PIXELS_PATH` / `LOD_CONFIG_BIAS_PATH` in `lod/lod_config.h:38-39`** (the `GLOBAL_DEF`s at `.cpp:357-358` read them but the *consumer* in `gpu_culler.cpp:218,222` uses string literals, so the macros themselves are unused). 2 LOC. Net diff = fix the misleading "Legacy LOD settings (kept for compatibility)" comment + delete the unused macros. Do NOT delete the `GLOBAL_DEF`s — consumers read those settings.
4. **Delete `tests/test_asset_dependency_manager.h` (10 LOC placeholder header).** One consumer: `tests/test_gaussian_splatting.h:20` `#include` it. Remove the include along with the file. The corresponding `.cpp` (which holds the actual coverage) continues to register tests via the `TEST_CASE` mechanism.
5. **Collapse the `parallel_build` stub in `lod/hierarchical_splat_structure.cpp:92-98`.** 7 LOC → 1 LOC. Either call `build_node_recursive` unconditionally, or delete the field from `BuildParams`.
6. **Delete the aspirational skipped `TEST_CASE` in `tests/test_gaussian_importer.h:596-...`.** ~40 LOC of setup below an unconditional early return.
7. **Delete the `#if 0 // Disabled for perf baseline` block in `render_debug_state_orchestrator.cpp:32-38`.** Replace the function body with `return false;`. 6 LOC.
8. **Retire the `pack_opacity` surface.** ~60 LOC across 8 files. Already telegraphed by `set_disabled(true)` CheckBox + "(Deprecated)" label. Coordinated — medium-risk churn.
9. **Delete the "Legacy renderer helpers" trampolines in `debug_overlay_system.h:167-179` + `.cpp:349-354,…`.** ~25 LOC of `func(GaussianSplatRenderer*) { func(build_command_sink(p_renderer)); }`. Requires migrating any external `GaussianSplatRenderer*`-flavor callers to `DebugOverlayCommandSink`.
10. **Delete the `// Legacy GPU depth sort shader path has been retired alongside instance-only compute shaders.` comment + the fall-through `return false;` at `gpu_sorting_pipeline.cpp:2783-2785`.** Just a breadcrumb. 3 LOC.

---

## Things that look legacy but aren't

- **`GS_LOCK_ORDER_GUARD`** (`core/gaussian_splat_manager.cpp:82-89`) — DEV_ENABLED-only macro. Intentional compiled-out defensive check, not legacy.
- **`GS_DEBUG_FREE_TRACKING`** (`renderer/gpu_debug_utils.h:180-234`) — active dev/ship toggle, both arms real.
- **`allow_legacy_resident_fallback`** in `GaussianSplatRenderer` — the word "legacy" names the non-streaming backend; the flag is *live* (`:1392: plan.allow_legacy_resident_fallback = !plan.streaming_requested`). Not dead code. Rename for clarity would be nice.
- **`gaussian_data.cpp:419 import_metadata["dc_encoding"] = "legacy_bias"`** — canonical asset-metadata value for the non-linear-RGB encoding. Not legacy code; legacy is just the string that names the encoding mode.
- **`interfaces/gpu_culler.cpp:1535 // Legacy path: requires gaussian_data or test positions`** — live fallback path for non-streaming assets. Not dead.
- **Headless-mode `MESSAGE("Skipping test - ...")` guards** — structural CI accommodation, not test-quarantine rot.
- **`_get_bool_setting`/`_get_int_setting`** in `gpu_debug_utils.h` — labeled "Legacy aliases" but used by 13 files actively. Renaming is pure diff.
- **`kWorldVersion = 1`** — single-version constant, no dead branches.

---

## Blind spots

- **Shader side.** I grepped shader `.glsl` files for markers but didn't walk shader dependencies exhaustively. The Bitonic/OneSweep shader strings are obviously abandoned (because the factory never reaches them), but other helper includes could have dead variants that a pure-grep sweep won't catch.
- **Python build scripts** (`SCsub`, `compile_shaders.py`). These could pull stale shader sources or reference deprecated flags. `shaders/compile_shaders.py:450-461` does contain `overflow_flag`/`unclamped_total` substitution — relevant to the stale-constant finding but not strictly legacy.
- **`doc_classes/*.xml`** documentation referencing properties that may have been renamed. I only spot-checked `GaussianSplatDynamicInstance3D.xml:7` (mentions the deprecated `ply_file_path`).
- **`docs/**/*.md`** — likely contains stale references. This audit is code-focused and didn't sweep docs.
- **The `editor/gaussian_editor_services.cpp` panel** — it still prints `"Pack Opacity: ..."` in asset summaries (`:271`). If the `pack_opacity` surface is retired, this line and its `dict_get_bool` above need deletion too. Flagged under the pack_opacity cleanup.
- **Downstream callers outside the module** — `scene/` / `editor/` / `platform/` may depend on the deprecated `GaussianData::get_rendering_device()` or the `ply_file_path` property via GDScript. I only grepped within `modules/gaussian_splatting/`. A wider sweep is needed before any user-facing API deletion.
- **Git-blame age skew.** I did not run `git log --since=` filtering. A properly staffed audit would flag files untouched for >18 months as candidates.

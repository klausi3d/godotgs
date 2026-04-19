# I/O Resource Importers — Deep Audit

Unit 07, lens: Godot resource-importer integration (ResourceFormatLoader / EditorImportPlugin lifecycle, metadata round-trips, .import files, UID stability).

Files in scope (cited throughout):

- `modules/gaussian_splatting/io/resource_importer_ply.h` / `.cpp`
- `modules/gaussian_splatting/io/resource_importer_spz.h` / `.cpp`
- `modules/gaussian_splatting/io/resource_importer_gsplatworld.h` / `.cpp`
- `modules/gaussian_splatting/io/gaussian_splat_world_io.h` / `.cpp`

Cross-references verified: `register_types.cpp`, `core/gaussian_data.h` (Gaussian layout / `get_gaussian_storage`), `core/streaming_chunk_payload_source.h` (`StagedFileChunkPayloadSource`), `interfaces/gpu_culler.h` (`StaticChunk`), `io/gaussian_import_preset.cpp` (preset table), `io/ply_loader.cpp` (`write_cache` / `.gsplatcache`).

---

## Summary

**Grade: C**

The importer set works and the basic Godot contract is honoured (`get_importer_name`, `get_save_extension`, `get_format_version`, registration in `register_types.cpp`). The binary `.gsplatworld` format has a reasonable header layout, range validation, and mirrors its on-disk structure between saver and loader. There is real thought behind the `.gsplatcache` vs `.gsplatworld` split and the deadlock rationale around `can_import_threaded()` is documented.

But the ceiling is low:

1. **PLY and SPZ importers are ~90% copy-pasted** (options table, option-getters, counting/indexing loop, compression flag builder, merge-stride math). There is no shared base or helper class — only the string namespaces differ. This is a maintenance bomb every time anything cross-cutting changes (already visible in option-key drift and feature drift between the two).
2. **The saver/loader path encodes GPU-alignment-sensitive raw `Gaussian` structs directly to disk** (`sizeof(Gaussian) == 144`, enforced by `static_assert`) and changes to that struct silently break existing `.gsplatworld` caches. The `kWorldVersion` constant is still `1`.
3. **ResourceUID plumbing is entirely absent.** All three `import()` implementations begin with `(void)p_source_id;`. This breaks UID stability in ways the Godot 4 importer contract expects — any tooling that relies on stable UIDs for cross-scene references is relying on luck.
4. **Integer overflow on attacker-controlled offsets** in the bounds checks of both `_validate_gsplatworld_header` (importer) and `ResourceFormatLoaderGaussianSplatWorld::load`.
5. **`.gsplatworld` is a self-describing binary blob that also happens to be the "save extension" of its own importer** — meaning `import()` does not transform the data, it just copies the file, then re-opens it via `ResourceLoader::load(..., CACHE_MODE_IGNORE)`. Importing a world is a glorified `_copy_binary_file` with a header sanity check. Fine, but not advertised in API naming.

Nothing here is likely to crash the editor *today* on well-formed input. On malformed input or across a future `Gaussian` layout change the picture is worse.

---

## What this code does

Three `ResourceImporter` subclasses live under `io/`:

1. **`ResourceImporterPLY`** (`resource_importer_ply.cpp:236`) — Wraps `PLYLoader`, re-indexes gaussians for optional opacity sort + density subsampling, emits a `GaussianSplatAsset` as `.tres` at `p_save_path`, registers the sidecar `.gsplatcache` (written by `PLYLoader::write_cache`) in `r_gen_files`.
2. **`ResourceImporterSPZ`** (`resource_importer_spz.cpp:212`) — Same flow, but sources from `SPZLoader` and embeds SPZ-header metadata (version / SH degree / fractional_bits / antialias flag) in the asset metadata dictionary.
3. **`ResourceImporterGSplatWorld`** (`resource_importer_gsplatworld.cpp:227`) — Validates the header, copies the source file verbatim into `p_save_path + ".gsplatworld"`, then round-trips via `ResourceLoader::load(..., CACHE_MODE_IGNORE)` to prove the copy is decodable. This is an unusual identity-importer pattern.

Alongside, `gaussian_splat_world_io.cpp` implements `ResourceFormatLoaderGaussianSplatWorld` / `ResourceFormatSaverGaussianSplatWorld`. Together they encode a 104-byte header + raw `Gaussian[]` payload (optionally gzip-compressed) + high-order SH coeffs + chunk table + chunk indices + optional JSON metadata. These are the canonical bytes for `.gsplatworld` on disk and are also how `PLYLoader::try_load_cache` / `write_cache` persists the parsed PLY result under a renamed `.gsplatcache` extension.

Registration happens in two phases in `register_types.cpp`: format loader/saver at `MODULE_INITIALIZATION_LEVEL_SCENE` (line 148–159), importers at `MODULE_INITIALIZATION_LEVEL_EDITOR` under `TOOLS_ENABLED` (line 163–177). Uninitialization mirrors this symmetrically.

---

## Strengths

- **Clear contract surface.** Each importer correctly overrides `get_importer_name`, `get_save_extension`, `get_format_version`, `can_import_threaded`, `get_priority` (implicit default except gsplatworld). Registration lifecycle in `register_types.cpp:148-235` is symmetric and uses `Ref<>` to keep the format loader alive.
- **Format version gate exists.** `get_format_version() == 2` on all three importers (`resource_importer_ply.h:49`, `resource_importer_spz.h:45`, `resource_importer_gsplatworld.h:27`) with a clear comment pointing to the `_ensure_buffer_sizes()` zero-init fix. This is the right Godot mechanism for invalidating caches on importer logic changes.
- **Binary .gsplatworld bounds checks do exist**, if flawed — `_validate_gsplatworld_header` in `resource_importer_gsplatworld.cpp:30-141` checks magic, version, SH degree clamp, `_checked_mul_u64` overflow guard on `splat_count * sizeof(Gaussian)` and `splat_count * sh_high_order`, `_range_in_file` for all regions. Better than most hand-rolled binary loaders.
- **GSplatWorld importer validates round-trip.** After copying, it reopens via `ResourceLoader::load(save_path, "GaussianSplatWorld", CACHE_MODE_IGNORE, ...)` (`resource_importer_gsplatworld.cpp:253`) and deletes the output on decode failure (line 259-261). That defensive delete is a nice touch.
- **`_read_exact` slices reads at 256 MiB** (`gaussian_splat_world_io.cpp:41-57`) — correctly acknowledges MSVC CRT `fread` limitations on >2 GiB buffers. Matching 256 MiB slicing on the write side (line 579-589).
- **Deadlock root cause is documented.** `resource_importer_ply.h:26-33` and `resource_importer_spz.h:36-38` explain why `can_import_threaded()` returns false — synchronous `RenderingServer::texture_2d_get` round-trip inside `--headless --import`. Keeps future maintainers from optimistically flipping the flag.
- **Legacy option keys are preserved with fallback.** `_get_bool_option(p_options, OPTION_VALIDATE, OPTION_VALIDATE_LEGACY, ...)` in `resource_importer_ply.cpp:53-64` tolerates pre-namespaced `.import` files. The `pack_opacity` deprecation path (line 296-300) correctly warns instead of silently consuming.
- **Chunk index overflow guarded.** `resource_importer_gsplatworld.cpp:122` and the `ERR_PRINT` in `gaussian_splat_world_io.cpp:346-353` explicitly defend against `chunk.indices_offset + chunk.index_count` overflowing `uint64_t`.

---

## Top issues

**[severity: corruption]** `modules/gaussian_splatting/io/gaussian_splat_world_io.cpp:241,251,266` — Bounds checks use `sh_offset + sh_bytes > file_len`, `chunk_table_offset + chunk_table_bytes > file_len`, `metadata_offset + metadata_size > file_len`. All three are addition-based and wrap on attacker-controlled `uint64_t` offsets. The validator in the importer (`resource_importer_gsplatworld.cpp:93-138`) correctly uses `_range_in_file` (`offset` and `size <= file_size - offset`) but the *load path* that runs at runtime re-implements these checks badly. A `.gsplatworld` with `metadata_offset = UINT64_MAX - 10, metadata_size = 20` passes the load check. — This is a file-corruption / potential OOB read vector on every user-loaded world. — Mirror `_range_in_file` from the importer into the loader (or share one helper). Same pattern: check `offset <= file_len && size <= file_len - offset`.

**[severity: corruption]** `modules/gaussian_splatting/io/gaussian_splat_world_io.cpp:207` — `const uint64_t gaussian_bytes = uint64_t(splat_count) * sizeof(Gaussian);` has no overflow check in the *loader*. The importer validator does use `_checked_mul_u64` but the loader path (used at runtime and from `try_load_cache`) does not. `splat_count = 0x20000000` × `sizeof(Gaussian)=144` = ~46 GB; wraps at `splat_count ≥ 2^57 / 144`. — Feasible only with a malicious file, but once wrapped the `LocalVector<Gaussian>::resize(splat_count)` at line 275 allocates the un-wrapped size and the read goes past buffer. — Use `_checked_mul_u64` (or promote it to a shared header).

**[severity: corruption]** `modules/gaussian_splatting/io/gaussian_splat_world_io.cpp:463-644` (`ResourceFormatSaverGaussianSplatWorld::save`) — **No magic + version pair is tied to `sizeof(Gaussian)`**. The Gaussian struct is asserted to be 144 bytes (`core/gaussian_data.h:179`), and the saver writes `gaussian_bytes = splat_count * sizeof(Gaussian)` raw into the file. Any future layout change — adding a field, reordering for alignment — silently breaks every existing `.gsplatworld` and `.gsplatcache` on disk but `kWorldVersion` stays `1`. The `_ensure_buffer_sizes()` zero-init fix cited in the importer bumped `get_format_version()` for .tres, but **there is no analogous on-disk version gate for .gsplatworld binary payloads**. — Users re-opening old caches after a codebase bump get garbage splats, not a clean error. — Either (a) introduce a `gaussian_layout_hash` field in the header and fail on mismatch, or (b) stop encoding raw POD structs and write a schema'd payload (field-by-field).

**[severity: corruption]** `modules/gaussian_splatting/io/resource_importer_gsplatworld.cpp:244-263` — After `_copy_binary_file`, the importer calls `ResourceLoader::load(save_path, "GaussianSplatWorld", CACHE_MODE_IGNORE, &err)` to validate. But the load path itself (not the header validator) has the overflow bugs above. So validation is unsound against pathological input even though the importer looks defensive. — Attacker-controlled `.gsplatworld` passes header validation, triggers OOB read during round-trip. — Fix the loader bounds checks; the importer is fine.

**[severity: crash]** `modules/gaussian_splatting/io/gaussian_splat_world_io.cpp:169-170` — The saver's `store_buffer` return values are partially ignored. `_ensure_file_write_ok` checks `file->get_error()` after each section, but the compressed-path write on line 570 (`file->store_64(compressed_gaussians.size())`) is not followed by an error check before the slice-write loop. A write failure between `store_64` and the payload slices leaves a partially-written file that still passes the first `_ensure_file_write_ok` at line 584. — Half-written `.gsplatworld` on disk full / IO error. — Add `_ensure_file_write_ok(file, "save(gaussian_size_prefix)")` immediately after line 570.

**[severity: corruption]** `modules/gaussian_splatting/io/resource_importer_ply.cpp:344-349`, `resource_importer_spz.cpp:309-315` — Density-merge math: `merge_stride = original / final_count; start = floor(i * stride); end = floor((i+1) * stride)`. With FP rounding near the right edge, `end` can equal `start` → `CLAMP(end, start+1, original_count)` silently steps outside the intended range. More importantly, `indices_ptr[sample_index]` inside `_merge_gaussian_range` reads through an index that is always `start + count/2` — **but `count` is `end - start`, which is `1` most of the time**, so this is just picking the first of the range. The "pick representative splat" comment is misleading — in practice this is just stride subsampling. — Not a crash, but the stated intent ("avoid hole artifacts from blended positions") isn't delivered. The code is effectively `i -> indices[floor(i * stride + stride/2)]`, which is fine but hides behind helper indirection. — Either delete `_merge_gaussian_range` and inline a clear subsampler, or actually implement representative-point selection (e.g., pick max-opacity within the bin).

**[severity: maint]** `modules/gaussian_splatting/io/resource_importer_ply.cpp:25-132` vs `resource_importer_spz.cpp:25-112` — `_get_bool_option`, `_get_int_option`, `_get_double_option`, `_get_string_option`, `_compute_final_splat_count`, `_merge_gaussian_range`, `_build_compression_flags` are **duplicated verbatim** in both translation units. The PLY variant has an extra `p_fallback` overload for legacy option keys; SPZ never grew that. The option-table builders in `get_import_options` are ~95% identical — PLY has `OPTION_VALIDATE` / `OPTION_WARN_MISSING` / `OPTION_CUSTOMIZED` / `OPTION_PACK_OPACITY`, SPZ does not. — Every new field or preset attribute requires two edits; they are already out of sync (`OPTION_CUSTOMIZED` is PLY-only, also absent from SPZ's `option_dict` build). — Extract a `GaussianAssetImportBase` (or free helpers in `gaussian_import_shared.{h,cpp}`) containing option descriptors, the option-dict builder, density-subsample loop, and metadata builder. The importer classes become thin façades selecting the loader.

**[severity: maint]** `modules/gaussian_splatting/io/resource_importer_ply.cpp:239`, `resource_importer_spz.cpp:215`, `resource_importer_gsplatworld.cpp:230` — **All three importers `(void)p_source_id;`**. Godot 4's `ResourceUID::ID` is the mechanism for stable cross-scene references; discarding it at the importer level means the importer never participates in UID registration. In most cases Godot's own `EditorFileSystem` handles this transparently, but the omission is universal — no code in this module ever calls `ResourceUID::get_singleton()->set_id(...)` or emits a `uid` field into `r_metadata`. — Low-impact today, but silently prevents UID-stability work down the line (rename tracking, cross-scene references to imported assets). — Document the decision explicitly (one-line comment: "Godot's EditorFileSystem handles UID registration via the .import sidecar — we do not customise UIDs") or actually wire it in. Today, neither.

**[severity: maint]** `modules/gaussian_splatting/io/resource_importer_ply.cpp:170-226` — `get_import_options` exposes options with `Variant::STRING` + `PROPERTY_HINT_ENUM` hint of `"mobile,desktop,high,ultra,development,custom"`. But `gaussian_get_import_preset_by_name("custom")` falls back to index 1 (desktop) silently (`gaussian_import_preset.cpp:129-134`). The "custom" enum value has **no corresponding preset**. Selecting it in the Godot inspector produces desktop behaviour without warning. — User confusion: "I selected Custom but it's doing Desktop." — Either add a `custom` preset (mirror desktop with a flag), or drop it from the enum hint and rely on `OPTION_CUSTOMIZED` + advanced dialog instead.

**[severity: maint]** `modules/gaussian_splatting/io/resource_importer_ply.cpp:228-234`, `resource_importer_spz.cpp:204-210` — `get_option_visibility` is a no-op that returns `true`. Godot calls this to hide options that don't apply to the current preset (e.g., `quality/max_splats` should be hidden when `preset == "ultra"` since ultra is unlimited). — UI shows fields that have no effect; max_splats = 750000 set on an ultra asset gets overridden because ultra preset returns `max_splats=0` anyway (wait — actually the user-set option wins per `_get_int_option`). So the semantic is silently inverted: "preset" selects defaults but per-option overrides always win, and visibility hint never communicates that. — Users think the preset is authoritative; it's only a default source. — Either honour `get_option_visibility` (hide overridden fields based on `preset != "custom"`) or document the "preset sets defaults, manual options always win" semantic in the inspector help text.

**[severity: corruption]** `modules/gaussian_splatting/io/gaussian_splat_world_io.cpp:383-400` — In `ResourceFormatLoaderGaussianSplatWorld::load`, chunk indices are sliced via `all_indices.ptr() + size_t(src_offset)`. On 32-bit builds (if anyone ever built that way; Godot 4 is primarily 64-bit but still), `uint64_t(src_offset)` → `size_t` truncation is lossless on 64-bit but a silent truncate on 32-bit. Combined with the fact that `total_indices` is computed via `MAX<uint64_t>(total_indices, record.indices_offset + record.index_count)` — if any chunk's `indices_offset` is sparse/high but total allocation fits, the `all_indices.resize(total_indices)` is correctly sized, but then the write-side at `save` always writes tightly packed (line 622-633). So a loader reading a file with artificially inflated `indices_offset` values will allocate much more than needed and read garbage into the buffer. — This is a mismatch between saver (packs indices tightly) and loader (honours `indices_offset` as a count index into the file region). — Either document that `indices_offset` *must* equal the sum of preceding `index_count` values and validate that invariant on load, or stop using a cursor field at all and just stream chunks sequentially.

**[severity: perf]** `modules/gaussian_splatting/io/resource_importer_ply.cpp:315-326`, `resource_importer_spz.cpp:279-290` — `Vector<int> indices` is populated with `[0..original_count)` then conditionally sorted by opacity. For `original_count = 5M` this is ~20 MB of `int` allocations per import **even when `sort_by_opacity` is false**. The sort-by-opacity path is opt-in (default false) — the identity indices array is built unconditionally. — Editor import of a 5M-splat PLY allocates 20 MB it immediately discards. — Guard with `if (sort_by_opacity) { ... build indices ... }` and pass a direct-access closure to the extraction loop.

**[severity: perf]** `modules/gaussian_splatting/io/gaussian_splat_world_io.cpp:515,528` — `bool use_compression = _is_world_compression_enabled() && gaussian_bytes > 1024;`. The setting is re-read *per save* via `ProjectSettings::get_setting_with_override` inside `GaussianSplattingIO::get_bool_setting`. This is cheap but unnecessary; more importantly, the `1024` threshold is a magic number with no config exposure. Larger problem: when compression is enabled, the entire uncompressed gaussian array is copied once into `Compression::compress`'s output buffer (sized to `get_max_compressed_buffer_size`) and then the result is `resize`'d down. For a 1 GB payload this is a doubled peak memory footprint during save. — OOM risk on large worlds with compression flag enabled. — Stream-compress in 256 MiB slices matching the write-side slice loop, or disable compression over a size threshold.

**[severity: maint]** `modules/gaussian_splatting/io/gaussian_splat_world_io.cpp:371` — `gaussian_data->set_gaussian_payload(gaussians, sh_high_coeffs, sh_first_order, sh_high_order, (flags & kFlagIs2D) != 0);` — `sh_first_order` is **read from disk but never validated**. The header stores it (line 188) and passes it through. `get_sh_first_order_count()` is the complement used by the saver (line 477). If a malicious file sets `sh_first_order = 0xFFFFFFFF`, downstream renderer code that indexes arrays by it will OOB. — Partial trust in header fields; validator clamps `sh_degree` and `sh_high_order` (via `_checked_mul_u64` against splat count) but nothing bounds `sh_first_order`. — Validate `sh_first_order + sh_high_order` against the SH-degree-expected count (e.g., at `sh_degree=3`, total SH count per splat is 15; first_order is typically 3).

**[severity: maint]** `modules/gaussian_splatting/io/resource_importer_gsplatworld.cpp:252-263` — `ResourceLoader::load(save_path, "GaussianSplatWorld", CACHE_MODE_IGNORE, &imported_decode_err)` during `import()` is functionally a test harness — it decodes the file the importer just wrote to verify integrity. But it costs a *full load* (which for a 1 GB world includes full gaussian buffer allocation + optional decompress) just to discard the result. Over-cautious on large assets; gratuitous on small ones. — Doubles import time on large worlds. — Only validate the header+offsets (which `_validate_gsplatworld_header` already does before the copy). Drop the full `ResourceLoader::load` round-trip, or keep it only in debug builds via `DEV_ENABLED`.

---

## Cross-cutting patterns

- **Two sources of truth for `.gsplatworld` bounds validation.** `_validate_gsplatworld_header` (importer, `resource_importer_gsplatworld.cpp:30`) uses correct `_range_in_file` helpers and `_checked_mul_u64`. `ResourceFormatLoaderGaussianSplatWorld::load` (`gaussian_splat_world_io.cpp:138`) re-implements them with naive `A + B > file_len` checks. The loader is what actually runs at game runtime; the importer only gates initial import. **The runtime path is less safe than the import-time path.** Unify on the safer helpers.
- **Copy-paste between PLY and SPZ importers.** The files diverge for no principled reason — PLY has legacy-key fallback, SPZ does not; PLY has `OPTION_CUSTOMIZED`, SPZ does not; SPZ has `source_format` metadata key, PLY does not. The duplication is not hygienic; it's the residue of one file having been edited and the other not. See P0 refactor below.
- **Raw POD serialization as a de-facto format spec.** `sizeof(Gaussian) == 144` is load-bearing for disk layout, but that invariant lives in `gaussian_data.h` and is enforced only by `static_assert`. The `kWorldVersion == 1` has not been bumped despite multiple importer-level `get_format_version()` bumps. The on-disk format and the importer cache-invalidation version are decoupled in a way that will produce corrupt reads the day someone widens `Gaussian`.
- **Importer discards UID, editor discards thumbnail, loader discards round-trip checks — a lot of discarding.** The code is defensive where it's easy (magic bytes) and indifferent where it matters (UID stability, post-save thumbnail caching, actual struct-layout versioning).
- **Thumbnail path couples import to render thread.** Every PLY/SPZ import instantiates `GaussianThumbnailGenerator` synchronously (`resource_importer_ply.cpp:386-389`), which is precisely the code path that forces `can_import_threaded() = false`. Same code is duplicated twice in the import body (once for the asset thumbnail, once for memory stats at line 447-452). If the thumbnail is the threading blocker, at minimum extract it to a post-import pass.
- **Metadata dictionaries are constructed with `StringName(...)` everywhere except the top of the function.** PLY uses `OPTION_PRESET` (SNAME'd) as a dictionary key on line 396, then `StringName("source_file")` on line 418. Mixed style. Low stakes, but inconsistent.

---

## Recommended refactor moves

### P0 (required before any feature work lands)

1. **Fix loader bounds checks to use range-safe arithmetic** (`gaussian_splat_world_io.cpp:241,251,266` and `sh_bytes` multiplication on line 207).
   - Effort: 1 hour. Extract `_range_in_file` + `_checked_mul_u64` from `resource_importer_gsplatworld.cpp` into a shared header (`io/gsplatworld_bounds.h`), use in both places.
2. **Tie `.gsplatworld` on-disk version to `Gaussian` layout.**
   - Effort: 2–4 hours. Add a `gaussian_struct_size` field to the header (or a hash of Gaussian layout), reject loads on mismatch with a clear migration error. Bump `kWorldVersion` to 2 and document the version bump policy.
3. **Post-save `store_buffer` error check after `store_64(compressed_gaussians.size())`** (`gaussian_splat_world_io.cpp:570`).
   - Effort: 15 minutes.

### P1 (next sprint)

4. **Extract PLY/SPZ common code into `gaussian_asset_importer_base.{h,cpp}`.** Put the option-getter helpers, option-dict builder, metadata builder, density-merge loop, and preset-name resolution in one place. The two importer `.cpp` files shrink to ~80 lines each (loader choice + header boilerplate).
   - Effort: 4–8 hours. Also unblocks P1-5.
5. **Make SPZ honour the legacy-key fallback pattern already in PLY.** Add `OPTION_PACK_OPACITY` deprecation warning, `OPTION_CUSTOMIZED`, `OPTION_VALIDATE`, `OPTION_WARN_MISSING` to SPZ — or document that these are PLY-specific.
   - Effort: 1 hour after P1-4.
6. **Stop building `[0..N)` identity indices when `sort_by_opacity == false`** (`resource_importer_ply.cpp:316-320`, SPZ equivalent).
   - Effort: 1 hour.
7. **Document the UID decision.** Either implement custom UID registration or add an explicit comment stating the delegation to `EditorFileSystem`.
   - Effort: 30 minutes.

### P2 (when touched anyway)

8. **Honour `get_option_visibility` for preset-driven visibility** — hide `max_splats` when preset is `ultra`, hide density when preset is `high`, etc.
   - Effort: 2 hours.
9. **Replace full-load post-import round-trip with header-only re-validation** (`resource_importer_gsplatworld.cpp:252-263`).
   - Effort: 1 hour.
10. **Drop or re-implement `_merge_gaussian_range`** — currently misleading (comment claims representative-splat selection, implementation picks stride midpoint).
    - Effort: 30 minutes (delete + inline), or 2 hours (actually implement max-opacity pick).
11. **Add/validate the `custom` preset** — either add a concrete definition to `gaussian_import_preset.cpp` or remove it from the enum hint.
    - Effort: 30 minutes.

---

## Blind spots

- **Large-world round-trip not stress-tested.** The code has 256 MiB slice loops for `get_buffer`/`store_buffer` but I did not find an integration test in `tests/test_gaussian_splat_world_io.h` exercising >2 GiB files. This is claimed-but-unverified robustness.
- **I did not read `PLYLoader` parse internals** (`ply_loader.cpp` beyond the `write_cache` / `try_load_cache` section) — only the cache interaction with `ResourceFormatLoaderGaussianSplatWorld`. Parser-level bugs are out of slice (Unit 08 territory).
- **SPZ format fidelity is trusted from `SPZLoader`.** The importer reads `SPZLoader::get_header()` and stores header fields in metadata verbatim (`resource_importer_spz.cpp:388-392`). Whether `SPZLoader` validates the header is outside this slice.
- **`.import` sidecar file semantics.** I verified `get_format_version()` return values match across importer overrides, but did not exhaustively test the scenario where a user with a pre-v2 `.ply.import` file upgrades — the Godot `EditorFileSystem` scanner is trusted to trigger re-import, per the version bump comments.
- **Thread-safety of `StagedFileChunkPayloadSource`** (set from `load` at `gaussian_splat_world_io.cpp:425-431`) — its `mutable Mutex file_mutex` is out-of-slice but critical to the correctness claim of "file-backed streaming". If `configure` is called from one thread and `capture_chunk_snapshot` from another, ordering matters.
- **Editor integration (`GaussianImportSettingsDialog::get_singleton`)** — `show_advanced_options` calls into an editor singleton; I did not verify its lifetime vs the importer. If the dialog is unregistered before the importer, clicking "advanced" in the inspector hits a null.
- **UID behaviour across rename.** `p_source_id` is discarded, which in practice means Godot re-issues a UID when a `.ply` file is renamed. Consequences on scene references that use `uid://...` paths to gaussian assets are unexplored.
- **`ProjectSettings` read from `ResourceFormatSaverGaussianSplatWorld::save`.** `_is_world_compression_enabled()` reads `ProjectSettings::get_singleton()` during save. If saving happens before the setting is registered (e.g., during module init, or from a worker thread at a weird point in startup), the setting returns its fallback silently. Not a likely issue in practice, but the assumption is undocumented.

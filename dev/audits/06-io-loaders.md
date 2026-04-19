# I/O Loaders — Deep Audit

## Summary

**Grade: D+**

This is the module's trust boundary — every byte walks in from disk, and an
attacker (or a merely careless tooling chain) gets to choose them. The good
news: several hardening passes have clearly happened. `spz_loader.cpp` has
explicit `_mul_u64_overflow` / `_add_u64_overflow` helpers, fixed safety caps
(`MAX_SPZ_COMPRESSED_BYTES`, `MAX_SPZ_DECOMPRESSED_BYTES`, `MAX_SPZ_POINTS`),
offset-range validation (`_offset_range_valid`), and gzip trailer
cross-checking. `gaussian_splat_world_io.cpp` bounds-checks every section
against `file_len` before seeking, and explicitly guards the `indices_offset +
index_count` addition against `UINT64_MAX` overflow (line 346).

The bad news: `ply_loader.cpp` is the weak link. `header.vertex_count` is a
signed `int` filled from a 64-bit `to_int()` return value with *no* range
check, *no* sign check, and *no* upper bound. A malicious or corrupt PLY can
drive negative loop bounds, allocate gigabytes, or silently truncate the
count. The cached path (`try_load_cache`) then treats the same un-bounded
count as a sanity oracle against cache contents — two-wrongs-make-a-right.
Endian swapping in `read_float_property` type-puns through `uint32_t*` (strict
aliasing UB). `parse_ascii_data` loops on file-supplied count without
reverifying remaining file content. The cast `static_cast<int>(total_bytes)`
at line 532 is gated, but the gating is subtle and easy to break in the next
refactor.

`gaussian_data_loader.cpp` is a fine thin dispatcher but has no magic-number
sniff — it dispatches solely on extension, so a `.ply` file whose first line
is `ply` but payload is SPZ-shaped goes to the wrong loader. `i_gaussian_loader`
is a stub interface class whose only real code path (`ResourceFormatLoaderGaussianSplat::load`)
delegates to `GaussianSplatAsset::load_from_file` which lives outside this
slice.

The presets file (`gaussian_import_preset.cpp`) is trivial data and fine.

**Bottom line:** SPZ and `.gsplatworld` would survive a modest fuzzing
campaign. PLY would not.

## What this code does

1. **`ply_loader.{h,cpp}`** — parses ASCII and binary PLY files (little-
   and big-endian) used by 3D Gaussian Splatting (3DGS). Maintains a property
   table from the header, allocates `GaussianData`, reads position / scale /
   rotation / opacity / normal / palette / SH coefficients. Has a
   bidirectional `.gsplatcache` sidecar mechanism: parses header fast,
   attempts to load a prebuilt `GaussianSplatWorld` cache keyed on
   `(source_size, source_mtime, cache_version)`, falls back to full parse,
   writes cache on success. Migrates legacy `.gsplatworld` caches in place.

2. **`spz_loader.{h,cpp}`** — parses Niantic's SPZ compressed format (two
   variants: raw SPZ header + gzipped payload, or entirely gzip-wrapped).
   16-byte header (magic `NGSP`, version 2/3, sh_degree, fractional_bits,
   flags). Payload encodes positions as 24-bit fixed-point, alphas as u8,
   colors as RGB u8, scales as 8-bit log-encoded, rotations as 3-byte v2
   smallest-three or 4-byte v3 smallest-three, plus SH quantized to u8.
   Has explicit overflow math and 512 MiB / 1 GiB caps.

3. **`gaussian_splat_world_io.{h,cpp}`** — reads and writes the native
   `.gsplatworld` binary format: 104-byte header (magic `GSPW`, version,
   flags, splat count, SH info, bounds AABB, chunk count, 6 × u64 section
   offsets/sizes) + raw or gzip-compressed `Gaussian` struct array + optional
   SH-high coefficients + optional spatial chunk table + optional chunk
   indices + optional JSON metadata blob. This is both the raw-save target
   *and* the PLY cache sidecar target.

4. **`gaussian_data_loader.{h,cpp}`** — thin free-function dispatcher:
   picks SPZ vs PLY by lowercase extension, returns `GaussianDataLoadResult`
   with missing-property diagnostics (PLY only).

5. **`i_gaussian_loader.{h,cpp}`** — declares a (mostly unused) abstract
   `IGaussianLoader` base and a concrete `ResourceFormatLoaderGaussianSplat`
   that routes `.ply` / `.spz` through `GaussianSplatAsset::load_from_file`
   (implementation out-of-slice).

6. **`gaussian_import_preset.{h,cpp}`** — static table of five import
   presets (mobile / desktop / high / ultra / development). Pure data.

7. **`io_settings_utils.h`** — helper for reading bool `ProjectSettings`
   values.

## Strengths

- **SPZ payload-size computation is properly gated.** `_compute_expected_payload_bytes`
  (spz_loader.cpp:45–70) uses explicit overflow-checked multiply/add and the
  caller rejects both `> MAX_SPZ_DECOMPRESSED_BYTES` (1 GiB) and
  `> UINT32_MAX` (lines 228–235). The preliminary-audit claim that the
  `uint32_t(payload_size)` cast at line 255 is unchecked is **refuted** —
  line 232 explicitly gates it. Good defensive coding.
- **`.gsplatworld` loader bounds-checks every section against `file_len`
  before seeking** (gaussian_splat_world_io.cpp:200–272). The chunk-indices
  path even guards `indices_offset + index_count` against `UINT64_MAX`
  overflow at line 346.
- **`_read_exact` slices large reads to 256 MiB** (gaussian_splat_world_io.cpp:41–57)
  to dodge a real MSVC CRT bug with `fread` > 2 GiB. Shows the author has
  hit this in practice.
- **SPZ gzip framing is parsed defensively** (spz_loader.cpp:414–573) with
  explicit handling for extra-field, filename, comment, and CRC16 subfields,
  each bounds-checked against `trailer_start`. Trailer size is
  cross-validated against the expected payload.
- **Fallback decompression strategy** — if `MODE_GZIP` yields the wrong
  length, the loader retries with raw DEFLATE from stripped frame
  (spz_loader.cpp:553–572). Handles zlib compile-mode quirks silently
  rather than erroring.
- **Cache versioning** (`PLY_CACHE_VERSION = 1`) combined with
  `(source_size, source_mtime)` gating and `CACHE_MODE_IGNORE` resource-load
  prevents stale cache hits within a session (ply_loader.cpp:229–302).
- **PLY ASCII mode has a strict-parse toggle** (ply_loader.cpp:56–61, 648)
  — invalid numeric tokens abort instead of silently defaulting to zero.

## Top issues

- **[severity: crash]** `modules/gaussian_splatting/io/ply_loader.cpp:179` —
  `header.vertex_count = parts[2].to_int();` assigns `int64_t` (String
  return) to `int` (struct member, ply_loader.h:61) with no validation. A
  PLY declaring `element vertex 3000000000` silently truncates to
  `-1294967296`; `element vertex -1` passes untouched. Downstream loops
  (line 539, 608) use `int i < header.vertex_count` so the result is either
  no iterations (silent empty import) or a several-GB allocation in
  `gaussian_data->resize(header.vertex_count)` at line 340. **Why it
  matters:** first-hop trust boundary, one-line DoS/crash primitive against
  any tool that consumes an externally-supplied `.ply`. **Fix:** parse as
  `int64_t`, reject `< 0`, cap at e.g. 200M (or derive from file size /
  sizeof(min vertex)), then narrow to `int` with an assertion.

- **[severity: crash]** `modules/gaussian_splatting/io/ply_loader.cpp:532, 553`
  — `bulk_buffer.resize(static_cast<int>(total_bytes))` and
  `chunk_buffer.resize(static_cast<int>(batch_bytes))` narrow `uint64_t` to
  `int`. The bulk path is gated by `total_bytes <= INT_MAX` at line 530,
  but the chunk path at 553 does not guard `batch_bytes`. Since
  `max_vertices_per_chunk = 16 MiB / vertex_size`, `batch_bytes <= ~16 MiB`
  in practice, so this is safe *today*. It's a ticking maintainability
  landmine — the moment someone bumps `target_chunk_bytes` past 2 GiB
  or changes the formula, this silently produces a truncated allocation
  with a full-length read. **Fix:** use `Error err = resize(int64_t)` via
  Vector's native 64-bit overload; drop the cast entirely.

- **[severity: corruption]** `modules/gaussian_splatting/io/ply_loader.cpp:900–923`
  — `read_float_property` type-puns a `float` via `uint32_t *int_val =
  (uint32_t*)&value;` and modifies `*int_val`. Strict-aliasing UB per
  C++ standard (type punning between float and uint32_t is not permitted
  through cast, only via `memcpy` or `std::bit_cast`). MSVC is permissive,
  GCC/Clang at `-O2+` may reorder or elide the store. Same pattern at
  lines 915 (double/uint64), 903 (float), 964 (int32). **Why it
  matters:** produces silently-wrong endianness conversion on release
  builds once the codebase moves to a stricter compiler / LTO /
  PGO pipeline — the bytes are read correctly, the swap *vanishes*.
  **Fix:** use `std::memcpy` into a `uint32_t` temporary, byte-swap,
  `std::memcpy` back — or use `std::byteswap` (C++23) / intrinsics.
  `__builtin_bswap32` everywhere non-MSVC.

- **[severity: crash]** `modules/gaussian_splatting/io/ply_loader.cpp:193–209`
  — PLY property `prop.size` is set only for recognised type strings; for
  anything else (e.g. `list`, custom types, typos) `prop.size` defaults to
  `0` from the default initializer. The code at line 189 *does* `continue`
  on `list` but no `continue` on unknown types — so an unknown typed
  property is pushed with `size = 0`, which means `vertex_property_offset`
  does not advance, so subsequent properties overlap in the vertex layout.
  Binary data is then read from the wrong offsets. **Why it matters:**
  silent corruption on any PLY containing a property type the loader
  doesn't know (e.g. `long long`, `ulong`, future extensions). **Fix:**
  treat unknown types as a hard error — they make the vertex layout
  ambiguous.

- **[severity: corruption]** `modules/gaussian_splatting/io/ply_loader.cpp:158–220`
  — the header parse loop terminates when `eof_reached()` fires *or*
  `end_header` is seen. If a malformed header lacks `end_header`, the
  loop exits on EOF with `header.header_size == 0` (never assigned),
  `header.vertex_count` possibly set from a prior element, and the loader
  then seeks / reads from position 0 as if the "binary payload" started
  at byte 0. This mis-parses the header as payload. **Fix:** require
  `end_header`; reject if loop ends without it.

- **[severity: crash]** `modules/gaussian_splatting/io/ply_loader.cpp:573–574`
  — `gaussian_data->resize(header.vertex_count)` is called before the
  ASCII row-count is validated. If vertex_count is 2 billion and the
  file is empty after the header, `resize()` allocates ~1.6 TB
  (`sizeof(Gaussian) * 2^31`). OOM / allocator abort. Same issue at line
  340 for binary. **Fix:** cap against a sanity bound derived from file
  size (bytes remaining / min-bytes-per-vertex).

- **[severity: crash]** `modules/gaussian_splatting/io/spz_loader.cpp:263`
  — `compressed_data.resize(compressed_size)` where `compressed_size =
  file->get_length() - data_start`. Neither checked for
  `file->get_length() < data_start` (would yield gigantic `uint64_t`
  under-flow → crash allocation) nor for `compressed_size >
  MAX_SPZ_COMPRESSED_BYTES`. The compressed stream's absolute size *was*
  gated earlier at line 125 (`file_size > MAX_SPZ_COMPRESSED_BYTES`),
  but `data_start = file->get_position()` could theoretically be larger
  than `file->get_length()` after a seek if the backend is non-monotonic.
  Narrow risk, but `get_buffer(ptr, compressed_size)` on line 264 has no
  return-value check either. **Fix:** check `data_start <= file_size`;
  check `get_buffer` return matches; bound against caps.

- **[severity: crash]** `modules/gaussian_splatting/io/spz_loader.cpp:150–151`
  — `compressed_file.resize(file_size); file->get_buffer(compressed_file.ptrw(), file_size);`
  — `file_size` is `int64_t` checked `> 0` and `<= MAX_SPZ_COMPRESSED_BYTES`
  (line 125), so the resize is safe. *But* the `get_buffer` return value
  is thrown away. A partial read leaves uninitialized tail bytes in the
  compressed buffer — gzip parsing will detect it, but an attacker can
  coerce work (buffer allocation, partial decompression) with a
  truncated file. **Fix:** check the return equals `file_size`.

- **[severity: crash]** `modules/gaussian_splatting/io/spz_loader.cpp:95–106`
  — `is_spz_file` reads 4 bytes without checking the return of
  `get_buffer`, and does not handle a file whose length is between 4 and
  `sizeof(SPZHeader)`. The line-95 guard `file->get_length() <
  sizeof(SPZHeader)` rejects under-sized files, but the same function
  is a *static* gate used by callers who might read a 4-byte magic stub
  (e.g. editor import sniff). No crash today, but return value discipline
  is inconsistent with the rest of the loader. **Fix:** verify
  `get_buffer` returned 4; fail closed on short read.

- **[severity: corruption]** `modules/gaussian_splatting/io/gaussian_splat_world_io.cpp:384–397`
  — chunk-indices bounds check uses `src_offset <= all_indices.size()` and
  `src_size <= all_indices.size() - src_offset`. Good. But `src_size` is
  `uint64_t(record.index_count)` — `index_count` is `uint32_t` so the
  conversion is fine. However, the comparison mixes `uint64_t(src_offset)`
  against `uint64_t(all_indices.size())` where `all_indices.size()` is
  `int` (Vector::size returns int64_t, but here narrowing from `int` if
  compiler picks wrong overload). The ternary on line 386 is safe, but
  the diagnostic message on 394 prints `all_indices.size()` as `%d` — a
  huge (theoretically 4B-entry) chunk index table would print negative in
  the error log, confusing recovery. Minor, but indicative. **Fix:**
  canonicalise index counts as `size_t` / `int64_t` end-to-end.

- **[severity: crash]** `modules/gaussian_splatting/io/gaussian_splat_world_io.cpp:406`
  — `PackedByteArray metadata_bytes = file->get_buffer(metadata_size);` —
  return value discarded. If the file is truncated between the section
  header check (line 266–272) and the actual read (e.g. racy truncation
  on network FS), `metadata_bytes.size() < metadata_size` and
  `String::utf8` receives a short buffer. No crash immediately (utf8
  handles short), but the downstream JSON parse produces a
  silently-wrong metadata dict. **Fix:** check returned size.

- **[severity: perf]** `modules/gaussian_splatting/io/ply_loader.cpp:573–747`
  — ASCII parsing is `O(N × property_count × strcmp)` because it
  `if (prop.name == "x") … else if (prop.name == "y") …` per column per
  vertex. With 100 vertices × 50 properties that's 5000 string compares.
  A 200k-vertex file with 60 properties = 12M string compares. The binary
  path hoists the indices once before the loop; the ASCII path should do
  the same via a pre-built property-dispatch table. **Fix:** build an
  `enum PropKind` LUT keyed by property index, switch on it.

- **[severity: maint]** `modules/gaussian_splatting/io/gaussian_data_loader.cpp:9–47`
  — dispatch is by lowercase file *extension* only. No magic-byte sniff,
  no fall-through ("this said .ply but content is SPZ-like"). An
  adversarial tool could rename an SPZ to `.ply`, send it through, and
  trigger PLY parser behaviour on SPZ-shaped bytes — which *might* survive
  (unlikely) or might hit a bug we haven't found. **Fix:** sniff first
  128 bytes; dispatch on magic, fall back to extension.

- **[severity: maint]** `modules/gaussian_splatting/io/ply_loader.cpp:193, 197–209`
  — PLY property type parsing is a cascade of string-equality comparisons.
  `"uchar"` / `"uint8"` / `"float32"` / `"float"` tables repeat in
  `read_float_property`, `read_uint_property`, and `parse_binary_data`.
  The duplication is the bug source for issue-4 (unknown types silently
  emit size=0). **Fix:** single enum-keyed LUT, build once per header.

- **[severity: maint]** `modules/gaussian_splatting/io/ply_loader.cpp:229–302`
  — `try_load_cache` uses `ResourceFormatLoaderGaussianSplatWorld` via a
  *stack-allocated* `format_loader` object rather than the registered
  loader, to dodge ResourceLoader's in-memory cache. Works but fragile —
  anyone who adds state to the format loader (thread-local state, singleton
  registration) will silently break this. A comment mentions the reason
  (line 247–249) but the approach bypasses normal Godot patterns. **Fix:**
  add `CACHE_MODE_IGNORE` to the real ResourceLoader call, or stop
  abusing ResourceFormatLoader for sidecar deserialisation and expose a
  `GaussianSplatWorld::deserialise(path)` static.

- **[severity: crash]** `modules/gaussian_splatting/io/spz_loader.cpp:140`
  — `file->get_buffer(first_bytes, 2);` without return-value check. If
  the file is exactly 1 byte, `first_bytes[1]` is uninitialized, and the
  GZIP magic check at line 145 reads it. The guard at line 133
  `file_size < sizeof(SPZHeader)` (16 bytes) prevents this today. But if
  that guard moves or is bypassed, crash. **Fix:** check return.

## Cross-cutting patterns

1. **Mixed size types.** `int` (PLY vertex_count), `uint32_t` (SPZ
   num_points, vertex offsets), `uint64_t` (byte counts, offsets), `int64_t`
   (FileAccess sizes, String::to_int return), `size_t` (stdlib). Casts
   ripple across the call graph. Each cast is individually defensible,
   but collectively they create many independent places where a refactor
   can introduce silent narrowing.

2. **Return value discipline on `get_buffer` is inconsistent.** The
   `.gsplatworld` loader's `_read_exact` helper gets it right. SPZ
   `load_file` mostly checks (line 534 for bulk). PLY `parse_header`,
   SPZ `is_spz_file`, and SPZ `load_file` (line 140, 151, 264) silently
   discard it. `FileAccess::get_buffer` returns the actual number of
   bytes read — dropping it converts a truncated-file error into
   use-of-uninitialized-memory.

3. **Unknown/unexpected type strings default silently.** PLY unknown
   property types get `size=0`. PLY ASCII unknown property names are
   silently ignored. SPZ unrecognised version is rejected explicitly —
   the good pattern.

4. **Endian handling via type-punning.** All endian-swap sites in
   `read_float_property` / `read_uint_property` use
   `uint32_t *x = (uint32_t*)&value` which is strict-aliasing UB. The
   byte-order logic itself is correct.

5. **Header parse fails open.** PLY `parse_header` returns OK if
   `vertex_count > 0` after loop exits — even if `end_header` was
   never observed, even if `properties.empty()`. The later binary
   parser catches the empty-properties case (line 336) but not the
   no-end-header case.

6. **Hard-coded 1 GiB / 512 MiB / 33M-point SPZ caps with no
   project setting.** A legit 50M-point scene trips the limits with
   no override path. A malicious file is rejected. Reasonable defaults,
   but worth surfacing in `ProjectSettings`.

7. **`memcpy` with user-controlled size.** `gaussian_splat_world_io.cpp:387`
   — `memcpy(chunk.indices.ptrw(), all_indices.ptr() + src_offset,
   record.index_count * sizeof(uint32_t))`. Guarded by the preceding
   bounds check, so safe. The pattern repeats across the file and
   should be consolidated into a `safe_memcpy_from(src, src_len,
   src_offset, dst, dst_len, count)` helper.

## Recommended refactor moves

**P0 (blocker, hours-to-days):**

1. **Harden PLY vertex_count parsing.** Parse as `int64_t`, bound by a
   configurable `MAX_VERTEX_COUNT` (default 200M), reject negative, reject
   non-numeric, reject overflow *before* any downstream allocation. Apply
   the same bound as a post-check against `gaussian_data->resize()` return
   (currently resize returns `Error` from `Vector<T>::resize`, ignored).
   Cost: ~1 day including test fuzz corpus.

2. **Replace all `(uint32_t*)&float` type-punning with `std::memcpy`.**
   Mechanical. Cost: ~2 hours. Non-optional for release-build
   correctness once compiler/flags advance.

3. **Require `end_header` in PLY.** One `bool saw_end_header` flag,
   one check at the bottom of `parse_header`. Cost: 15 minutes.

4. **Canonicalise size-type discipline.** All byte counts / offsets /
   element counts threaded through the loaders as `int64_t`. One cast
   at the trust boundary (file read), one cast to `Vector::Size` at
   allocation. No `static_cast<int>(uint64_t)` sprinkled at call sites.
   Cost: 1–2 days.

**P1 (important, days):**

5. **Add magic-byte sniff to `load_gaussian_data_from_file`.** Read first
   16 bytes, dispatch on `GSPW` / `NGSP` / `ply\n`, fall back to extension.
   Cost: 2 hours.

6. **Build a PLY property-dispatch LUT.** Enum-keyed instead of
   `strcmp`-cascaded. Speeds up ASCII parser by ~50×, eliminates the
   size=0-on-unknown-type footgun. Cost: 4 hours.

7. **Consolidate bounds-checked `memcpy` via helper.** `safe_slice_copy`
   in a shared header, used by `.gsplatworld` chunk-index path, SPZ
   payload section parse, any future format. Cost: 2 hours.

8. **Fuzz corpus + CI target.** AFL/libFuzzer entrypoint calling
   `PLYLoader::load_file` / `SPZLoader::load_file` /
   `ResourceFormatLoaderGaussianSplatWorld::load` against a curated
   malformed corpus. Minimum-viable: 200 hand-crafted edge cases
   (negative counts, off-by-one section boundaries, u32 overflow at
   sh_bytes, truncated gzip stream). Cost: 1 week, gives decades of
   mileage.

**P2 (nice-to-have, hours):**

9. **Surface SPZ safety caps via `ProjectSettings`.** `rendering/
   gaussian_splatting/import/spz_max_points`,
   `spz_max_compressed_bytes`, etc. Cost: 30 minutes.

10. **Deduplicate PLY property type string comparisons.** Single
    `PLYPropertyType` enum, single `property_size(PropType)` function.
    Cost: 2 hours.

11. **Document the sidecar cache format-loader workaround.** Either move
    `.gsplatcache` deserialisation out of ResourceFormatLoader (cleaner)
    or add a regression test that instantiates a stack-allocated
    `ResourceFormatLoaderGaussianSplatWorld` against a hand-written
    cache blob. Cost: 2 hours.

## Blind spots

- **`GaussianData::resize` / `set_gaussian` / `set_spherical_harmonics`
  implementations** are out-of-slice (core/). The loader assumes `resize`
  will either allocate or fail; if it silently clamps (as some templates
  do), the loop in `parse_binary_data` will write past the actual
  allocation. I didn't verify the resize semantics.

- **`GaussianSplatAsset::load_from_file`** (core/) is the real work done
  by `ResourceFormatLoaderGaussianSplat`. The parent-level file
  (`i_gaussian_loader.cpp`) just delegates to it, so anything this slice's
  class might be vulnerable to depends on that implementation.

- **`StagedFileChunkPayloadSource`** is instantiated at
  `gaussian_splat_world_io.cpp:425–430` but lives in `interfaces/`. If it
  trusts the `(gaussian_offset, sh_offset, splat_count, …)` it receives
  verbatim, any trusting-a-user-int bug in *this* loader propagates. Its
  trust model is not audited here.

- **`String::to_int()`** return on overflow. I assume it saturates or
  wraps silently (`to_int("99999999999999999999")`). If it already
  guards against overflow, issue #1 is partially mitigated — but only
  for the overflow subset, not the negative or near-INT_MAX cases.

- **MSVC `fread` > 2 GiB** — referenced by `_read_exact` as a known bug.
  I haven't verified the exact ceiling (MSVC CRT, Godot's FileAccess
  variant) matches the 256 MiB slice chosen. If the actual ceiling is
  higher, the slicing is just a perf tax.

- **`Compression::decompress`** behaviour on mismatched expected size —
  the SPZ decompressor retries with DEFLATE if `MODE_GZIP` yields wrong
  size; I assume `decompress` returns `-1` or similar on malformed
  input, not UB. Not verified against the Godot core implementation.

- **Thread safety.** All loaders are stateful (`header`, `gaussian_data`
  members). Concurrent calls to `load_file` on the same instance will
  race. The resource-format-loader entry points are called by Godot's
  importer, whose threading model I didn't check against this slice.

- **ASCII float parsing locale.** `String::to_float` may or may not
  respect the C locale's decimal separator (comma vs period). A PLY
  generated on a `de_DE` system could be silently mis-parsed. Not
  verified — if `to_float` is locale-independent (likely), this is a
  non-issue.

---

**Files audited (8):**
- `modules/gaussian_splatting/io/ply_loader.cpp` (1069 lines)
- `modules/gaussian_splatting/io/ply_loader.h` (117 lines)
- `modules/gaussian_splatting/io/spz_loader.cpp` (846 lines)
- `modules/gaussian_splatting/io/spz_loader.h` (160 lines)
- `modules/gaussian_splatting/io/gaussian_splat_world_io.cpp` (662 lines)
- `modules/gaussian_splatting/io/gaussian_splat_world_io.h` (28 lines)
- `modules/gaussian_splatting/io/gaussian_data_loader.cpp` (48 lines)
- `modules/gaussian_splatting/io/gaussian_data_loader.h` (18 lines)
- `modules/gaussian_splatting/io/i_gaussian_loader.cpp` (74 lines)
- `modules/gaussian_splatting/io/i_gaussian_loader.h` (36 lines)
- `modules/gaussian_splatting/io/gaussian_import_preset.cpp` (136 lines)
- `modules/gaussian_splatting/io/gaussian_import_preset.h` (32 lines)
- `modules/gaussian_splatting/io/io_settings_utils.h` (49 lines)

**Verification spot-checks (5):**
- Confirmed `PLYHeader::vertex_count` declared as `int` (ply_loader.h:61).
- Confirmed `String::to_int()` returns `int64_t` (core/string/ustring.h:465).
- Confirmed `Vector<T>::Size == int64_t` (core/templates/cowdata.h:50).
- Confirmed `LocalVector<T, U=uint32_t>` so `.resize()` parameter is
  `uint32_t` by default (core/templates/local_vector.h:43).
- Confirmed `PackedByteArray = Vector<uint8_t>` (core/variant/variant.h:77).

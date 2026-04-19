# GPU Sorting — Deep Audit

> **Auditor:** 25+ yr senior render engineer. Ruthless lens on sort correctness,
> GPU-side edge cases, async timeline semantics, and quantization math.
>
> **Unit 14 scope (files, LOC):**
> - `renderer/gpu_sorter.cpp` (3,413), `renderer/gpu_sorter.h` (487)
> - `renderer/gpu_sorting_config.cpp` (586), `renderer/gpu_sorting_config.h` (154)
> - `renderer/gpu_sorting_constants.h` (17)
> - `renderer/sh_config.cpp` (177), `renderer/sh_config.h` (85)
> - `renderer/quantization_config.cpp` (331), `renderer/quantization_config.h` (92)
> - `renderer/float16_config.cpp` (207), `renderer/float16_config.h` (77)
> - `renderer/float16_utils.cpp` (314), `renderer/float16_utils.h` (214)
> - Total: **6,154 LOC** of C++.
>
> All GLSL bodies are embedded in `gpu_sorter.cpp` as raw-string literals — this
> audit treats them as C++ concerns because they are built and dispatched from
> this file. Actual `.glsl` files are Units 16/17, see **Blind spots**.

## Summary

**Grade: C.**

The skeleton is competent: there is a factory, a policy resolver, three backends
(radix / bitonic / onesweep), device-generation guards on RID lifetime, and a
preflight validator. But once you drill past the surface, the implementation is
rotten where it matters most for a depth sort:

- The **OneSweep and Bitonic shaders sort `float` keys**. The radix path forces
  64-bit composite tile+depth keys. `OneSweepSort` and `BitonicSort` cannot
  service the instance pipeline at all — they would silently reorder garbage if
  the factory ever selected them. The factory knows this and routes around it
  (`requires_indirect = true`, `requires_64bit_keys = key_config.key_bits > 32`
  at `gpu_sorter.cpp:949-950`), but nobody has deleted the dead paths and the
  shaders themselves still compile-and-dispatch on `float` comparisons.
- **Bitonic's power-of-two pad region is never initialized.** `padded_count`
  elements are swapped; the range `[count, padded_count)` contains whatever was
  in the buffer previously. That garbage is sorted into the valid range.
- **OneSweep is not a stable sort.** Its scatter uses `atomicAdd` on per-digit
  offsets (`gpu_sorter.cpp:2958`) — any two equal-digit keys land in
  arbitrary order, so the multi-pass LSD composition is wrong in general. The
  pass count (4) also assumes 32-bit keys; there is no 64-bit path.
- **Zero-count async returns a synthetic timeline value** without issuing any
  GPU work or fence. `sort_async` and `sort_async_with_timeline` both
  `return ++timeline_value;` with no submit (`gpu_sorter.cpp:2386, 2395`). Any
  downstream code that waits on the returned semaphore reads last frame's data.
- **The indirect count buffer is sized for 16 bytes but the indirect-dispatch
  shader reads 24.** The shader declares `overflow_flag` and `unclamped_total`
  (`gpu_sorter.cpp:2123-2124`); the buffer is created with
  `kIndirectDispatchHeaderSize` = `offsetof(overflow_flag)` = 16 bytes
  (`gpu_sorter.cpp:2094`). GPU OOB read, undefined but silent on most drivers.
- **Radix sort depth encoding is unspecified.** The CPU never sanitizes the
  depth values that go into the key; a negative or NaN view-space z will
  produce radix-sorted garbage positions. The audit prompt flagged this at a
  line now moved to a different offset — still live.
- **`float16_utils.cpp` rounding is wrong for the "round to nearest even" case.**
  The "sticky bit" mask is `0x2FFF` (`float16_utils.cpp:49`) — should be
  `0x1FFF`. Subtle: this biases a subset of tied halves upward. Catches one
  value in 8,192 on average.

Architecture of the config layer is acceptable but carries three footguns that
a reviewer will meet before they get to the sort:

- `sh_config.cpp:144-155` registers `-1` sentinel correctly; nothing else in
  the audit scope does. `float16_config.cpp` has no `register_project_settings`
  function at all — it just reads keys on load, so fresh projects see C++
  defaults and no UI.
- `gpu_sorting_config.cpp:567-571` uses `#ifdef DEBUG_ENABLED` to gate
  prefix-readback registration. In release builds these settings cannot be
  toggled without editing `project.godot` by hand — silent product trap.

## What this code does

1. **`GPUSorterFactory`** (`gpu_sorter.cpp:920-1125`) — given an `algorithm`
   enum (AUTO/RADIX/BITONIC/ONESWEEP), a `RenderingDevice*`, a max element
   count, and a `SortKeyConfig`, probes capabilities of all three backends,
   coerces to a policy that requires indirect dispatch + 64-bit keys, and
   returns a `Ref<IGPUSorter>`. Downgrades AUTO to RADIX for any workload that
   needs 64-bit keys or tie-break stability (`gpu_sorter.cpp:385-396`).

2. **`RadixSort`** (`gpu_sorter.cpp:1127-2618`) — 8-bit radix (configurable to
   4-bit) on 32- or 64-bit keys. Pipeline:
   histogram → wg_prefix → bin_prefix → scatter, ping-ponged between
   `keys_buffer` and `temp_keys_buffer`. Per-pass barriers. Uses a GPU-driven
   indirect count buffer to derive element_count inside shaders; also derives
   workgroup-count via a one-invocation shader (`indirect_dispatch_shader`,
   `gpu_sorter.cpp:2115-2149`). Exposes `sort`, `sort_indirect`,
   `sort_indirect_async`, `sort_async`, `sort_async_with_timeline`.

3. **`BitonicSort`** (`gpu_sorter.cpp:519-918`) — O(n log² n) parallel bitonic
   over `float keys[]`. Pads to next power of two. No indirect dispatch.
   Advertised for "< ~64K elements" but in practice gated out by factory.

4. **`OneSweepSort`** (`gpu_sorter.cpp:2620-3301`) — 4-pass 8-bit radix with
   a "chained scan" single-workgroup scan. `float keys[]`, non-stable scatter
   via `atomicAdd`. Also never reached from the instance pipeline.

5. **`SortKeyConfig` / `GPUSortingConfig`** — key bits (32/64), tile_bits,
   depth_bits, workgroup_size, radix_bits. Project-settings-backed with
   preset tiers (low/medium/high/ultra).

6. **`float16_utils`** — IEEE-754 binary16 ↔ binary32 conversion with NaN/Inf
   handling, per-chunk position quantization (subtract chunk center then store
   as halves), SH coefficient batch pack/unpack.

7. **`SHConfig`** — band-count selector (SH0..SH3) with sentinel `-1` for tier
   auto-seed. The only config file with source-attribution tracking
   (`sh_bands_source`).

8. **`QuantizationConfig`** — CPU-side *policy* struct for per-chunk position
   (8–24 bit) and scale (8–16 bit) quantization. **Important:** this file
   registers settings and reports compression ratios but does not implement
   the actual quantization — there's no codec here, just policy.

## Strengths

- **Device-generation tracking for RID ownership.** `uniform_owner_generation`
  + `ResourceOwnerMismatchContract::is_device_generation_valid` at
  `gpu_sorter.h:216-313` and used in `_free_uniform_sets_safe`
  (`gpu_sorter.cpp:264-279`). This is the right pattern for
  editor/device-recreation and directly addresses the class of bug behind
  issue #257.
- **Preflight validator with typed error codes.** `SortPreflightResult` /
  `SortPreflightError` at `gpu_sorter.h:85-103` and
  `_validate_sort_preflight` at `gpu_sorter.cpp:1240-1301`. Every failure
  mode has a name you can grep.
- **Buffer naming discipline.** Every RID gets
  `set_resource_name` — makes RenderDoc / Pix traces navigable
  (e.g. `gpu_sorter.cpp:2077-2101`). Rare in Godot modules.
- **Policy resolver is well-factored.** `evaluate_auto_policy`
  (`gpu_sorter.cpp:1054-1125`) builds fallback reasons with structured
  strings, not ad-hoc `vformat` soup.
- **`_cleanup_partial_init`** — symmetric teardown on init failure. The
  lambda-style "if this fails, clean up everything above" is the exact
  pattern most Godot modules get wrong. (`gpu_sorter.cpp:2177-2270`.)
- **SH config source attribution.** `sh_bands_source` / `sh_bands_source_label`
  in `sh_config.h:74-76` — end users can see whether SH level came from a
  tier preset or an explicit override. Should be copy-pasted to the other
  configs.

## Top issues

- **[crash]** `gpu_sorter.cpp:2115-2125, 2450` — **`indirect_dispatch_shader`
  declares its IndirectCount input block as 24 B** (`dispatch_xyz[3] +
  element_count + overflow_flag + unclamped_total`), but the caller-supplied
  `count_buffer` at `:2450` is typically 16 B (`kIndirectDispatchHeaderSize`,
  `pipeline_io_contracts.h:29` — `offsetof(overflow_flag)`). The radix
  histogram/scatter shaders at `:1617-1619, 1700-1702, 1829-1831` each
  declare only the first 16 B (`dispatch_xyz[3] + element_count`), but the
  indirect-dispatch preamble shader reads 24 B. The file's own
  `indirect_count_buffer` at `:2094` is also 16 B, so if a caller ever routes
  it through `_sort_indirect_internal` as `count_buffer`, the 8-byte read
  past end is active. Most drivers zero-pad; Vulkan validation with
  `robustBufferAccess=off` flags it as UB; on tight allocators this is a
  buffer-range-out-of-bounds validation failure. **Fix:** either align the
  shader layout with `kIndirectDispatchHeaderSize` (16 B, drop the unused
  `overflow_flag`/`unclamped_total` reads) or ensure all `count_buffer`
  allocations are `sizeof(IndirectDispatchLayout)` (24 B). Pick one canonical
  size and `static_assert` it in the shader too (via specialization constant).

- **[corruption]** `gpu_sorter.cpp:556, 759, 817, 626 (shader)` — **Bitonic
  power-of-two pad region is never cleared.** `padded_count = next_power_of_two(count)`,
  dispatch runs over `padded_count`, and the shader only early-exits when
  `j >= num_elements` (which IS `padded_count`). Threads with `i < count, j < padded_count`
  actively compare-and-swap the valid range against whatever uninitialized
  garbage happens to sit at `keys[count..padded_count-1]`. On re-sorts of the
  same buffer, stale data from previous frames migrates into the valid range.
  **Fix:** `rd->buffer_clear(keys_buffer, count*stride, (padded_count-count)*stride)`
  with sentinel `0xFFFFFFFF` for keys before the first dispatch — only when
  Bitonic is actually selected. Current factory paths (64-bit required) mean
  Bitonic is never reached from the instance pipeline, but `Bitonic::sort` is
  public API on `IGPUSorter` and will be invoked by tests or future call
  sites.

- **[corruption]** `gpu_sorter.cpp:2383-2387, 2391-2396` — **zero-count
  `sort_async` returns a fake, incremented timeline value without any GPU
  work.** `WARN_PRINT_ONCE` then `return ++timeline_value;` Downstream waiters
  treat the returned semaphore value as "this sort is done" and read last
  frame's sorted buffer. There is no real fence, and the previous sort's
  output is whatever pass finished last — so the renderer reads STALE DATA as
  if it were fresh. **Fix:** either issue a no-op compute dispatch with a
  barrier (cheap, real timeline), or return a distinguished sentinel
  (e.g. `UINT64_MAX`) that callers know means "no work queued". Also audit
  all `sort_async*` return values at call sites; any site that does
  `wait_for_value(return)` on a fake ticket is a silent hazard.

- **[corruption]** `gpu_sorter.cpp:2958` — **OneSweep scatter is not stable.**
  `uint output_pos = atomicAdd(local_offsets[digit], 1);` serializes all lanes
  with the same digit but produces output positions in *atomic arbitration*
  order, which is hardware-dependent and non-deterministic. LSD radix
  composition requires stability; without it, keys with equal low digits in
  pass N will scramble the relative order they had from pass N-1, so the
  final sort is correct in `digit[3]` but garbage in the tie-breaking lower
  bits. For Gaussian splats at the same tile, this means depth ordering is
  lost — exactly the thing the sort exists to preserve. **Fix:** implement
  the scatter as an exclusive-prefix-sum of a per-digit bit vector, the
  same pattern used by the (correct) RadixSort scatter at
  `gpu_sorter.cpp:1850-1905`.

- **[corruption]** `gpu_sorter.cpp:2681, 2749, 2907, 2911, 2715, 2790, 2955` —
  **OneSweep and Bitonic shaders compare `float keys[]`.** Raw `float > float`
  comparison in Bitonic (`gpu_sorter.cpp:602`) does not match
  `floatBitsToUint` radix ordering used downstream — negative floats sort
  *opposite* numeric order under raw bit radix, and the integer-wrapping
  tile+depth composite keys are not floats at all. OneSweep's
  `uint ikey = floatBitsToUint(key)` (`gpu_sorter.cpp:2715, 2790, 2955`) makes
  that assumption explicit: it cannot handle 64-bit tile-packed keys and
  cannot handle negative-z depths. The factory currently routes these out
  for the instance pipeline (`gpu_sorter.cpp:945-950`), so this is latent,
  not active — but the backends advertise `is_supported()` as true and will
  silently return wrong results if ever invoked directly (e.g. from a test
  or a new call site). **Fix:** either delete the Bitonic/OneSweep backends
  or rewrite them to take `uvec2` 64-bit keys consistent with RadixSort.

- **[corruption]** `gpu_sorter.cpp:2386, 2395` + **depth encoding gap** —
  neither this file nor any caller the audit could find clamps the depth
  value before packing it into the low `depth_bits` of the sort key. If the
  projection hands back a negative view-space z (splat behind camera) or a
  NaN (degenerate covariance), `floatBitsToUint`-style encoding produces
  bits that radix-sort *before* genuine splats with small positive z. The
  previous auditor flagged this at a moved line offset; the gap itself is
  still live. **Fix:** clamp z to `[near_plane, far_plane]` in the tile
  binning stage (Unit 16/17), **and** `DEV_ASSERT` the invariant on the CPU
  side of `sort_indirect` — if the upstream shader has a bug, you want a
  loud failure, not mis-sorted pixels.

- **[crash]** `gpu_sorter.cpp:1411-1414, 2542-2545` — **odd-pass fallback is
  detected after the full compute list has already been recorded.** The
  lambda returns `false` but by that point `for pass = 0 .. num_passes-1`
  has already emitted every dispatch and barrier. The caller's
  `compute_list_end` + skip-submit path (`:1431-1440`, `:2561-2571`) handles
  this by *dropping the entire sort*, which means the output buffer is
  whatever it was on entry — stale last-frame data, or worse, mid-sort
  temp-keys garbage if a previous partial run left temp in the input slot.
  In practice `key_bits=64` + `radix_bits=4` gives 16 passes (even), and
  `radix_bits=8` gives 8 (even), so this should never fire; but the
  invariant is checked, so someone already wrote a case where it could.
  **Fix:** check `num_passes & 1` at variant *creation* time (create_variant,
  `:1457`) and refuse to create an odd-pass variant; then promote the
  runtime WARN to an `ERR_FAIL` because it's unreachable.

- **[corruption]** `float16_utils.cpp:48-49` — **IEEE-754 round-to-nearest-even
  sticky mask is wrong.** `if ((mantissa & 0x2FFF) || (half & 1))` — bit 13
  (the round bit itself) is excluded because the check at `:48` already
  accounts for it, but bit 14 should NOT be in the sticky mask since that is
  the bit *above* the round bit and is being kept. `0x2FFF` includes bit 13
  (0x2000) and excludes the round bit 0x1000. Correct sticky mask is
  `0x1FFF` (bits 12..0 — everything *below* the round bit that is being
  dropped). The current mask biases ~1 value in 8,192 upward at the rounded
  position. Small per-value error; large aggregate over a million-splat
  scene when multiplied across SH coefficients. **Fix:** change to `0x1FFF`
  and add a round-trip test with known IEEE reference vectors.

- **[corruption]** `float16_utils.cpp:31-33` — **denormal shift is
  unbounded.** `int shift = 14 - exponent;` then `mantissa >>= shift;`.
  When `exponent < -10` the function returns early (line 25), but at
  `exponent == -9` shift == 23, which is *defined* (whole mantissa shifted
  out). At `exponent == -10` shift == 24 — also defined (zero). Safe, but
  there is no test. More pressing: the NaN-handling branch at line 38
  masks to `sign | 0x7E00 | (mantissa >> 13)` — if the input NaN has all
  lower 13 bits set and upper bits zero, the resulting half has mantissa 0
  → becomes **Infinity, not NaN**. **Fix:** after `mantissa >> 13`, OR-in
  a non-zero bit (e.g. `| 0x1`) to guarantee mantissa != 0 for NaN input.

- **[perf]** `gpu_sorter.cpp:1314-1320, 2410-2415, 3147` — **no
  device-lost polling in any sort path.** `safe_submit` and
  `safe_submit_and_sync` return Errors the sort ignores. If the GPU is
  reset (thermal, VRAM OOM, driver hang), the timeline semaphore never
  signals, `wait_for_completion` blocks on a dead device, and the render
  thread joins it. On Windows+NVIDIA this surfaces as editor freeze. **Fix:**
  check return from `safe_submit_and_sync`; on error, tear down the sort
  state and return a distinguished error to caller. A 5-minute timeout
  loop would be better than hang-forever.

- **[maint]** `gpu_sorter.cpp:1362, 1490` — **`histogram_offset = pass *
  histogram_stride` has no bounds check.** The invariant holds if
  `pass < max_num_passes` and `histogram_stride = max_workgroups *
  max_radix_size` was computed with the same variant, but both are derived
  state, and a future change to `select_variant` (e.g. secondary variants
  with different `radix_size`) breaks the invariant silently. **Fix:**
  `DEV_ASSERT(histogram_offset + variant->radix_size <= histogram_bytes /
  sizeof(uint32_t))` at each pass. One line, catches the GPU OOB before it
  corrupts unrelated memory.

- **[crash]** `gpu_sorter.cpp:87-93, 154-160` — **`_acquire_submission_device`
  returns nullptr if `GaussianSplatManager::get_singleton()` is gone**, but
  callers at `:744, 911, 1315, 2411, 3117` check `ERR_FAIL_NULL_V` and bail
  with Error codes, leaving `is_sorting = true` in half the code paths
  (e.g. `_sort_async_internal` sets `is_sorting = true` at `:1335`, then
  subsequent failures in `_create_pass_uniform_sets` DO reset it at `:1352`
  — but failures BEFORE `:1335` never reach it because they bailed out
  early). Future refactors that move the ordering will leak the flag.
  **Fix:** RAII scope guard (`at_exit { is_sorting = false; }`) or a single
  `return_with_error` helper that resets state.

- **[perf]** `gpu_sorter.cpp:3141-3147` — **`OneSweepSort::sort` does a
  full `safe_submit_and_sync` *per pass*** (and does three
  `buffer_clear`s just before the submit). Four passes = four GPU stalls
  per sort. The rest of the codebase explicitly avoided this pattern
  (e.g. `:2572-2574` "Blocking sync here was causing 15 FPS cap"); OneSweep
  is an older path that never got the same treatment. **Fix:** consolidate
  clears and dispatches inside a single compute list with barriers, one
  submit. Only matters if OneSweep is ever used, which it currently isn't
  — but deleting it is also a valid fix.

- **[maint]** `float16_utils.h:10-99` vs `quantization_config.h:1-92` —
  **two quantization systems with no arbitration.** `Float16Utils`
  implements its own per-chunk offset encoding
  (`QuantizationChunk` at `float16_utils.h:103-108`). `QuantizationConfig`
  defines a *different* system (min/max bounds per chunk, integer encoding
  with 8–24 bit depth). Neither references the other. A reader cannot tell
  which is actually used, or whether both are live. The audit found no
  call graph connecting `QuantizationConfig` to any codec implementation
  inside this scope — it reports compression ratios for a system that may
  not exist in code yet. **Fix:** clarify which is canonical and delete or
  prominently-mark the other.

- **[maint]** `gpu_sorting_config.cpp:567-571` — **prefix-readback settings
  are `#ifdef DEBUG_ENABLED`.** Release builds cannot enable them, but the
  load function at `:115-117` still reads the setting unconditionally. If a
  user hand-edits `project.godot` to turn on prefix readback, a dev build
  respects it; a release build silently discards. The only config path
  that diverges silently between build flavors. **Fix:** register the
  settings in all builds, gate only the *default* value on DEBUG_ENABLED.

- **[maint]** `float16_config.cpp:1-208` — **no
  `register_project_settings` function.** Compare to
  `quantization_config.cpp:226-315` which does full
  `set_custom_property_info` + enum hints. Float16 config loads its keys
  but never advertises them to the editor — a fresh project has no UI for
  turning Float16 on. Users must paste paths into `project.godot` by hand.
  **Fix:** copy the pattern from `quantization_config.cpp`.

## Cross-cutting patterns

- **"Register the escape hatch, never trigger it."** `SortPreflightError` has
  nine distinct codes. The preflight is called. The error codes route to
  string conversion and logs. But no caller the audit examined routes these
  into the contract framework at `renderer/instance_pipeline_contract.h`
  that Unit 9 flagged as "designed but not invoked." Preflight fails with
  `INVALID_COUNT_BUFFER`; renderer swallows a `0` and proceeds.
  **The sort is a microcosm of the whole module's contract discipline:
  structured failure reporting, unstructured failure handling.**

- **Timeline semantics are CPU-side fiction.** `std::atomic<uint64_t>
  timeline_value{0}` in every sorter (`gpu_sorter.h:222, 305, 406`) is
  incremented on CPU immediately after `safe_submit` with no GPU-side
  fence of the same value. The return value is a "sort was submitted"
  ticket, not a timeline-semaphore value. Callers that treat it as a real
  timeline value — particularly for cross-device synchronization — are
  racing. The audit found `sort_async_with_timeline` advertising
  parameters `p_wait_semaphore`, `p_wait_value`, `p_signal_semaphore`,
  `p_signal_value` (`gpu_sorter.h:363-364`) that the internal
  implementation (`gpu_sorter.cpp:1303-1455`) accepts but **never actually
  uses to wait or signal**. The parameters are logged names with no
  effect. This is API-shaped to imply timeline sync; it is in fact fire-
  and-forget.

- **Configs diverge in maturity.** `sh_config.cpp` has source attribution,
  sentinel-based tier seeding, and custom property info. `quantization_config.cpp`
  has custom property info but no source attribution. `float16_config.cpp`
  has neither and no `register_` call. `gpu_sorting_config.cpp` has
  GLOBAL_DEF but no property info — fields appear in the editor without
  hints or ranges. These were written over time by different hands; no
  review enforced consistency.

- **Float16 vs QuantizationConfig duplication.** Two separate compression
  systems, two separate config singletons, two separate project-settings
  trees. Neither references the other, and the audit could find no code
  that *reads* `QuantizationConfig::per_chunk_quantization` to actually
  quantize anything inside this scope. It may be consumed by unit 15 or
  16; from here it reads as documentation-ware.

## Recommended refactor moves

### P0 — fix before ship (days)

1. **Size `indirect_count_buffer` correctly.** `gpu_sorter.cpp:2094`. Either
   allocate `sizeof(IndirectDispatchLayout)` (24 B) or shrink the shader
   layout. 30-minute fix, prevents a GPU OOB read.
2. **Replace zero-count fake timeline with no-op dispatch or sentinel.**
   `gpu_sorter.cpp:2383-2396`. ~1 hour.
3. **Clear Bitonic pad region or delete BitonicSort backend.** `gpu_sorter.cpp:759`.
   If the factory path truly never reaches Bitonic, deleting it is safer
   than carrying broken code. If kept, pre-clear the tail.
4. **Remove or fix OneSweep.** `gpu_sorter.cpp:2620-3301`. As shipped the
   sort is not stable and cannot handle 64-bit keys. Deletion is the
   defensible choice.
5. **Fix `float_to_half` sticky bit and NaN preservation.** `float16_utils.cpp:49, 38`.
   Two literal-value changes. Add round-trip test.

### P1 — fix within a sprint (week)

6. **`DEV_ASSERT` histogram offsets and pass counts.** `gpu_sorter.cpp:1362, 2490`.
   One-line assertions at every pass to catch future invariant drift.
7. **Wire preflight errors into the contract framework.** When
   `SortPreflightError != NONE`, bubble up as `sort_violation` at the
   pipeline stage boundary. See Unit 9.
8. **Delete unused `p_wait_semaphore` / `p_signal_semaphore` parameters** or
   actually implement them. `gpu_sorter.cpp:1303-1455`. Ghost API is worse
   than a clean limitation.
9. **Unify `Float16Utils::QuantizationChunk` and `QuantizationConfig`** into
   a single codec with one config. Whichever system has live consumers
   wins; the other is cut.
10. **Add device-lost polling.** Check submit return, add a 5-sec hard
    timeout on `wait_for_completion`. Prevents the "editor hangs forever"
    class of field ticket.

### P2 — technical debt (weeks)

11. **Delete OneSweep and Bitonic entirely** once their callers are
    confirmed dead (the instance pipeline has required indirect+64bit for
    long enough that they are dead weight). Saves ~800 LOC and removes
    correctness liabilities that cannot be triggered today but will be
    if anyone ever adds a 32-bit or non-indirect sort site.
12. **Canonicalize sort-key construction.** Put depth clamping and NaN
    handling in a single CPU-side helper that writes the tile+depth
    composite. Shader assumes the clean value; contract documents it.
13. **Source-attribution everywhere.** Copy `sh_bands_source` /
    `sh_bands_source_label` pattern into `Float16Config`,
    `QuantizationConfig`, `GPUSortingConfig`. Debug builds can show
    "where did this setting come from" in the inspector.
14. **Register Float16 project settings with property info.** Cargo-cult
    the `register_quantization_project_settings` function from
    `quantization_config.cpp:226-315`.

## Blind spots

- **Actual `.glsl` files.** The raw-string-literal shader bodies I audited
  are built and dispatched from this file, but any standalone `.glsl` in
  `shaders/` or `compute/` is Unit 16/17. Of particular interest: the
  tile-binning stage that *writes* the depth key is not in this unit. The
  "uncheckable depth encoding" bug (Top issue #5) requires Unit 16/17 to
  verify; this audit confirms the CPU side does no clamping.
- **Call sites.** The audit did not trace which callers invoke
  `sort_async_with_timeline` vs `sort_indirect_async`; any
  "timeline parameter is ignored" liability is latent or active depending
  on whether any caller actually passes non-zero wait/signal values. Unit
  5 (`render_*_orchestrator.cpp`) owns that.
- **`QuantizationConfig` consumers.** No consumer of the per-chunk
  integer quantization exists in this scope; if it lives elsewhere, its
  correctness is someone else's audit.
- **Tile contention / load balancing.** Radix scatter assumes uniform
  distribution of digits. For tile-depth keys with many splats at the
  same tile, all lanes with the same high bits concentrate on a small
  subset of bins in early passes. The audit did not measure whether this
  causes measurable atomic contention in the `bin_masks` path
  (`gpu_sorter.cpp:1558-1592`).
- **Performance baselines.** The entire "GPU timing" infrastructure
  measures CPU submission time, not GPU execution (explicit acknowledgment
  at `gpu_sorter.h:63-68`). Any claim in this audit about relative perf
  is from reading shader complexity, not measurement.
- **Subgroup correctness on Intel/AMD.** The subgroup-ballot paths at
  `gpu_sorter.cpp:1539-1592` were reviewed for logic only; actual hardware
  behavior differs enough across vendors that the `_subgroup_prefix_forced_off`
  escape hatch exists. Subgroup-off fallback path is simpler and auditable;
  subgroup-on path requires a cross-vendor test bench to validate.

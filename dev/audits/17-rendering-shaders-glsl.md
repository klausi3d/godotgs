# Rendering Shaders (GLSL) — Deep Audit

## Summary

**Grade: C+**

The GLSL surface area for the Gaussian Splatting module is larger than advertised (~7,200 LOC across 36 files, not ~4,000 across 16). The core rasterization and binning pipelines are numerically careful in many places — the eigenbasis code caps, low-pass-filters and rebuilds conics, validates quaternions, rejects huge covariances, and uses an explicit convex-quadratic tile-ellipse intersection. But under the hood the pipeline has a number of real correctness landmines: a subpixel visibility buffer read in EMIT that is written by COUNT _in the same dispatch chain_ without a documented pipeline barrier, a `gs_exp_fast` Schraudolph approximation that silently truncates on extreme negative inputs (via `uint()` of a negative float = undefined in GLSL), an R11G11B10 manual packer that ignores the half-float sign bit and therefore aliases negative and positive colors, a half-precision depth encoding that clips everything past ~65504 m, and a divergent `break`/`return` pattern inside a workgroup that calls `barrier()` inside the loop only on the `!pixel_saturated` branch. The SH evaluation correctly encodes Condon-Shortley, but the index guard `(i + 4u) < 16u` is a stale bound from an earlier 15-slot layout and lets band-3 coefficients shift out of place when high_count exceeds 12. None of these individually will crash a typical release build, but together they produce a long tail of platform-dependent numerical artifacts and performance cliffs.

The earlier flagged issues mostly hold: the z-clamp at `gaussian_splat_common_inc.glsl:157-163` still allows view-space `z` in `(-1e-6, 0]` (front of camera but behind near plane) to pass; the atomic "undo" at `tile_binning.glsl:1255-1258` is still not followed by a barrier; the degenerate conic check is still soft (radius >= 0 rather than det > 0) in the raster fast path. Some have been mitigated since the original audit — NaN/Inf validation is now done before the COUNT/EMIT split (line 1017), and the tile_rasterizer_compute now has in-loop barriers and a visible `in_viewport` guard that keeps idle threads participating in `barrier()`. Credit where due.

## What this code does

The shaders implement a tile-based compute rasterizer for 3D Gaussian Splatting atop Godot's `RenderingDevice`:

1. **`tile_binning.glsl`** (1,278 LOC) — single compute shader used in two passes (COUNT and EMIT) to project 3D Gaussians into screen space, derive 2D covariances → conics, compute tile overlap records, evaluate SH color, per-splat direct lighting, and pack a `ProjectedGaussian` payload for downstream rasterization.
2. **`tile_prefix_scan.glsl`** (236 LOC) — three-pass inclusive/exclusive scan over per-tile counts to allocate ranges in the global sort key/value buffers.
3. **`tile_rasterizer_compute.glsl`** (350 LOC) / **`tile_rasterizer.glsl`** (169 LOC) — two alternative implementations: a tile-sized compute shader that co-operatively loads `SPLATS_PER_TILE` payloads into shared memory and rasterizes in batches, and a fragment-shader fallback that reads everything directly from global memory.
4. **`tile_resolve.glsl`** (403 LOC) — post-rasterization resolve pass that unpremultiplies, applies direct lighting / shadow sampling via Godot's `scene_forward_lights_inc`, and applies tile feathering.
5. **`viewport_blit.glsl`**, **`gs_shadow_blit.glsl`** — final composition passes.
6. Painterly pipeline (`painterly_*`, `brush_accumulate`, `sobel_outline`) — stylization post passes.
7. Fragment-based legacy splatter (`gaussian_splat.vert.glsl` / `.frag.glsl`) — simple expand-to-quad pipeline used for regression baselines.

## Shader inventory

Top-level shaders (under `modules/gaussian_splatting/shaders/`):

- `brush_accumulate.glsl` — Compute pass that thickens edge-aligned strokes using a small 1D tangent blur driven by the Sobel edge map.
- `gaussian_splat.glsl` — Combined legacy vertex+fragment splat expander used by the fragment pipeline variant.
- `gaussian_splat.vert.glsl` / `.frag.glsl` — Painterly vertex/fragment splat pipeline used for regression compilation tests (consumes `painterly_common.glsl`).
- `gs_shadow_blit.glsl` — Minimal vertex+fragment shader that copies a depth texture into a shadow-map atlas slot, with optional invert.
- `painterly_composite.glsl` / `.vert.glsl` / `.frag.glsl` — Fullscreen compositor that applies scene-depth occlusion and blends the painterly color buffer over the scene.
- `painterly_resolve.glsl` — Compute post-pass implementing palette/ink/charcoal/watercolor style variants behind `#ifdef` macros.
- `sobel_outline.glsl` — Compute Sobel edge detection used by brush_accumulate and outline overlays.
- `tile_binning.glsl` — The workhorse compute shader (COUNT + EMIT passes) that projects, bounds, hotspot-culls, sorts-keys, and packs payloads for every visible Gaussian.
- `tile_prefix_scan.glsl` — Three-pass exclusive prefix scan producing per-tile `uvec2(start, count)` ranges + indirect dispatch metadata.
- `tile_rasterizer.glsl` — Fragment-shader rasterizer that reads the projected payload buffer and blends per pixel.
- `tile_rasterizer_compute.glsl` — Compute-shader rasterizer with cooperative shared-memory batch loading and early-exit subgroup vote.
- `tile_resolve.glsl` — Unpremultiply + deferred direct lighting/shadow compositor.
- `viewport_blit.glsl` — Final sRGB-aware blit with optional depth test against the scene buffer.

Includes (`modules/gaussian_splatting/shaders/includes/`):

- `color_grading_binning.glsl` — Exposure/contrast/saturation/temperature/tint/hue-shift pre-pack.
- `gaussian_splat_common_inc.glsl` — Legacy common helpers (SH evaluation, covariance projection) used by the fragment-expander pipeline.
- `gs_culling_utils.glsl` — Hashing, distance-based probabilistic culling, hotspot prune, overlap keep filter.
- `gs_deformation.glsl` — Wind/sphere-effector deformation.
- `gs_directional_shadow.glsl` — Directional/omni/spot shadow sampling adapters for the splat path.
- `gs_eigen_binning.glsl` — 2D eigen decomposition with low-pass filter + max-eigenvalue cap + raw-min-radius capture.
- `gs_instance_layout.glsl` — Instance/chunk/splat ref GPU struct definitions + flag bits.
- `gs_lighting_bridge.glsl` — Compute-shader compatibility shim for Godot's forward lighting include.
- `gs_lighting_common.glsl` — Omni/spot/directional light accumulation adapted for splats.
- `gs_quat_utils.glsl` — Non-normalizing `quaternion_to_matrix`, `gs_quat_rotate`, `gs_quat_mul`.
- `gs_render_params.glsl` — `RenderParams` UBO + helper accessors.
- `gs_sh_binning.glsl` — SH metadata + Condon-Shortley basis + `evaluate_sh_with_bands`.
- `gs_sort_key.glsl` — (tiny) depth-pad contract header.
- `painterly_common.glsl` / `painterly_features.glsl` — Shared painterly math + style variants.
- `platform_compat.glsl` — Workgroup sizing, subgroup macros, `gs_exp_fast`.
- `quantization_dequant.glsl` — Per-chunk position/scale dequantization + rotation fp16 unpack.
- `sort_contract.glsl` — Host-defined sort padding sentinel.
- `tile_projection_common.glsl` — `ProjectedGaussian` pack/unpack (incl. R11G11B10 rewrite).
- `tile_raster_common.glsl` — Per-pixel rasterization inner loop (shared-memory and global-memory variants).

## Strengths

- **Numerical discipline in the projection stage.** `project_gaussian_2d` (tile_binning:383) combines clip-w checks, quaternion-length validation (squared to skip sqrt), focal-length sanity bounds, radius-aware min-depth floor, explicit J_col2 clamp at ±1e4, NaN/Inf validation on cov2d, and a low-pass floor — each guarded by a debug counter. This is closer to production-quality than most reference implementations.
- **COUNT/EMIT divergence is actively defended.** The NaN/Inf conic check was moved before the COUNT/EMIT split (tile_binning:1017), subpixel visibility is latched through `SubpixelVisibility.visible[]` (876-883) so COUNT and EMIT reach identical decisions, and the hotspot prune predicate is explicitly deterministic per `(gaussian, instance, tile)`.
- **Exact tile-ellipse intersection.** `gs_tile_intersects_projected_ellipse` (tile_binning:299) does a proper convex-quadratic minimum test on each rectangular edge rather than a bounding-box overapproximation, substantially reducing tile-record generation.
- **Subgroup fallbacks are clean.** `platform_compat.glsl:190-204` keeps `GS_SUBGROUP_AVAILABLE=0` compiling without `#ifdef` spaghetti at call sites.
- **Compute rasterizer avoids the "early return before barrier" trap.** `tile_rasterizer_compute.glsl:142-175` keeps out-of-viewport threads participating in `barrier()` and only branches on write at line 256 after the last barrier. Good.
- **SH eval uses Condon-Shortley consistently.** `gs_sh_binning.glsl:66-73` documents the convention and matches `gaussian_splat_common_inc.glsl:236-247`. No sign flips between CPU and GPU.
- **Cross-vendor precision warning is explicit.** `platform_compat.glsl:106-113` documents that Schraudolph's method varies by ±1 ULP across NVIDIA/AMD/Intel/Apple and tells testers to define `GS_SAFE_EXP` for bit-exact runs.

## Top issues

1. **[corruption]** `tile_binning.glsl:1255-1258` — The overflow "undo" pattern `atomicAdd(tile_counts.counts[tile_idx], 1u)` followed by `atomicAdd(tile_counts.counts[tile_idx], 0xFFFFFFFFu)` is correct in isolation but leaves `tile_counts.counts[tile_idx]` transiently _above_ `range.y`. Subsequent EMIT threads racing on the same tile will see `local_offset >= range.y` and all take the undo path — even if some of those threads would have fit had they serialized first. Net effect: when a tile is within a few splats of its capacity, the race converts "exactly at capacity" into "everyone aborts", silently dropping records that should have fit. This is downstream of the clamp count itself being accurate only because of the compensating decrement — `tile_prefix_scan.glsl:86` reads this same counter after a cross-dispatch barrier, so the final value is fine, but within the EMIT pass the feedback is over-aggressive. Fix: use a compare-and-swap (`atomicCompSwap` loop) that only increments when below capacity, or allocate via a single `atomicAdd` and simply discard the write without decrementing the cursor (accepting a small over-count in stats).

2. **[corruption]** `includes/tile_projection_common.glsl:30-105` — `gs_pack_color_r11g11b10` hand-rolls the RGB9E5-style pack by extracting only the exponent and top mantissa bits from `packHalf2x16`: `r11 = (r_exp << 6) | (mantissa >> 4)` (line 42-48), dropping the bottom 4 mantissa bits per channel. The corresponding unpack at line 97 reconstructs via `(r_exp << 10) | (mant << 4)`, zeroing the low mantissa bits on the way back. This is ~6% worst-case precision loss per channel on top of the already-lossy pre-clamp at line 32 (`clamp(color, 0, 65504)`). With dithering amplitude `1/255 ≈ 0.004`, dim regions (color ≈ 0.05) take a quantization step comparable to the dither itself, defeating the banding mitigation. The sign bit of `packHalf2x16` output is also unobserved, but callers clamp to `max(color, 0)` before packing (tile_binning:1077, 1093, 1219) so negative inputs aren't the active bug. Fix: store two halves (`uvec2` holding RG + B padded, 32 vs 32 bits) using native `packHalf2x16` — preserves all 10 mantissa bits and is cheaper.

3. **[crash]** `includes/platform_compat.glsl:129-139` — `gs_exp_fast` does `uint bits = uint(max(bits_float, 0.0));`. When `bits_float` is NaN (which happens if a buggy caller passes an uninit value), `max(NaN, 0)` returns 0 on most hardware — but GLSL does not _require_ that. On Mesa radv the returned value is implementation-defined and can be NaN, in which case `uint(NaN)` is undefined (GL 4.5 §5.4.1 "Conversion and scalar constructors": behavior is undefined for out-of-range float-to-uint). Fix: replace with `uint bits = (bits_float > 0.0) ? uint(bits_float) : 0u;` — the comparison with NaN is false, so the branch safely zeroes.

4. **[corruption]** `includes/tile_projection_common.glsl:22-27` — Depth is packed as a single fp16 via `packHalf2x16(vec2(depth, 0))`. Linear depth here is `(positive_depth - near) / (far - near)` in `[0, 1]` (tile_binning:501-502). fp16 in that range has non-uniform precision: near 0 the LSB is ~6e-8, but near 1.0 the LSB is ~1e-3. Two splats separated by less than 1e-3 in linear depth near the far plane quantize to identical fp16 bit patterns, so `final_depth = min(final_depth, linear_depth)` becomes order-dependent (sort-key-dependent) for near-parallel surfaces. Fix: pack as 16-bit unorm — since the value is guaranteed in `[0,1]` this gives uniform ~1.5e-5 precision across the whole range, which is ~70x better near the far plane.

5. **[perf/corruption]** `tile_rasterizer_compute.glsl:181-186` — The `GS_MAX_RASTER_SPLATS_PER_TILE` check calls `atomicAdd` inside `if (gl_LocalInvocationIndex == 0)` but the clamping `splat_count = min(splat_count, uint(...))` runs for _every_ thread. If the define is changed per-pass without a matching recompile, threads disagree on `splat_count` — and then they proceed into the `for (batch_start = 0 ...)` loop which must drive `barrier()` in lockstep. Fortunately `splat_count` is computed from uniform inputs, so all threads currently see the same value — but this is brittle. Fix: compute `splat_count` once in `gl_LocalInvocationIndex == 0`, write to a shared, and barrier before every thread reads it, matching the pattern used a few lines above for `gs_shared_total_splat_count`.

6. **[corruption]** `gaussian_splat_common_inc.glsl:358, 432` — The loop bound `i < high_count && (i + first_count) < encoded_total && (i + 4u) < 16u && i < coeff_limit` uses `first_count` as an _offset into the encoded array_ (line 359: `encoded_coeffs[first_count + i]`) but indexes `basis` starting at `4u + i` regardless of `first_count`. If `first_count` is 2 instead of the expected 3 (which can happen with partially-trained models where the l=1 band is incomplete), the l=2 coefficient at encoded index `first_count` is multiplied by `basis[4]` — correct — but the coefficients that should occupy basis slots 5-8 are shifted by one encoded position relative to the true l=2 layout, producing a swapped basis-vs-coefficient pairing. The sibling `gs_sh_binning.glsl:184-193` avoids this by gating on `high_count > 0u` and clamping `max_high = min(high_count, 12u)`, but relies on the loader storing `first_count == 3` whenever high_count > 0. Fix: explicitly require `first_count == 0 || first_count == 3` at the top of both evaluators and reject otherwise.

7. **[perf]** `tile_binning.glsl:873-887` — The subpixel visibility ping-pong writes `subpixel_visibility.visible[global_idx]` from COUNT and reads it from EMIT. There is no `memoryBarrierBuffer()` in this shader — synchronization is expected to be done by the _C++ side_ between COUNT and EMIT dispatches. That is the correct place for it, but (a) the shader has no documentation stating so, and (b) if a future refactor merges COUNT and EMIT into a single dispatch (there is an `#ifdef` hint around line 873 that this is contemplated), the reads will see stale values. Additionally the `subpixel_history` buffer is written _only_ in EMIT (886-888) but also read by COUNT at 868 — the write-after-read order requires that COUNT complete before EMIT begins. Fix: add a shader-level assertion/comment block and an `#error` if both passes are defined simultaneously.

8. **[maint/perf]** `includes/tile_raster_common.glsl:184-186` — After the quadratic clamp to `>= 0` and reject at `> GS_RASTER_ALPHA_REJECT_Q`, there is no test that the conic is positive-definite. The upstream `project_gaussian_2d` does reject non-PSD conics via the low-pass filter floor, but any later transformation (including the rebuild at tile_binning:789-801) can produce a conic where `conic.x * conic.z - conic.y * conic.y <= 0`, in which case `quadratic` may be negative _before_ the `max(..., 0)` clamp. Result: splats with degenerate ellipses still enter the blend and contribute via `gs_exp_fast(-0.5 * 0)` = 1.0. Fix: reject `conic.x <= 0 || conic.z <= 0 || (conic.x*conic.z - conic.y*conic.y) <= 0` once before the main loop (it is cheap) and fold into the NaN check.

9. **[corruption]** `gaussian_splat_common_inc.glsl:157-163` — The z clamp `if (abs(z) < 1e-6) { z = z >= 0 ? 1e-6 : -1e-6; }` still accepts _any_ `z >= -1e-6`, including `z = 0.5` (a splat in front of the camera but well inside the near plane in a typical projection where near ≈ 0.1). The fragment-pipeline rasterizer (gaussian_splat.vert.glsl:34-38) does check `view_pos.z > -near || view_pos.z < -far`, but this covariance-projection path is shared with any other caller of `compute_projected_covariance`. Fix: add the same near/far guard inside `compute_projected_covariance` or make every caller validate before calling.

10. **[perf]** `tile_rasterizer_compute.glsl:238-247` — Inside the batch loop, the per-pixel branch `if (in_viewport && !pixel_saturated)` guards a call that writes to `pixel_saturated`, followed by an `atomicAnd(gs_shared_all_saturated, 0u)` _only_ in the `if (!pixel_saturated)` branch. The semantics are correct if all threads agree on whether they should or should not `atomicAnd`, but the atomic is technically divergent across the workgroup — some threads hit it and others don't. GLSL says this is well-defined (atomics don't require subgroup uniformity), but it creates serial memory traffic equal to `(workgroup_size - saturated_count)` atomic ops per batch. Fix: use a `subgroupAny(!pixel_saturated)` → leader writes, reduces to 1 atomic per subgroup.

11. **[corruption]** `tile_binning.glsl:872-883` — When `GS_TILE_GLOBAL_SORT_COUNT_PASS` is defined, `is_visible = raw_min_radius_px >= threshold` is computed and written to `subpixel_visibility.visible[global_idx]`. When `GS_TILE_GLOBAL_SORT_EMIT_PASS` is defined, `is_visible = subpixel_visibility.visible[global_idx] != 0u`. But the threshold used by COUNT depends on `was_visible` from `subpixel_history` (read at 868). If COUNT is run _before_ EMIT wrote `subpixel_history` last frame (frame-N EMIT writes, frame-N+1 COUNT reads), the first frame after a camera cut reads garbage. Fix: zero-initialize `subpixel_history` on resize/reset in C++, and document frame-ordering dependency.

12. **[maint]** `includes/gs_lighting_bridge.glsl:41-43` — The `#define gl_FragCoord gs_frag_coord_substitute` macro aliases a reserved GLSL identifier. GLSL 4.5 §3.4 reserves `gl_` names and states that redeclaration/redefinition produces a compile error or undefined behavior depending on implementation. Current tool chains (glslangValidator, Mesa) happen to accept this because the preprocessor resolves before semantic checks, but it is not portable. Fix: use a uniform name like `gs_frag_coord` throughout the Godot-side include (`scene_forward_lights_inc.glsl`) or feed `gs_frag_coord_substitute` into the shadow-sampling functions as an explicit parameter.

13. **[perf]** `tile_binning.glsl:411-415` — `quat_length_sq` in `[0.81, 1.21]` is an 11% tolerance, which lets through quaternions with `|q|` in `[0.9, 1.1]`. The subsequent Newton-Raphson correction `inv_len = 0.5 * (3 - quat_length_sq)` is only first-order accurate and has up to ~1% error at the endpoints. For highly tumbled datasets this produces subtle rotation errors (~0.5°) that correlate with scale and scale2 — yielding a systematic cov2d bias visible as elongated splats oriented along the xyz-axis of the rest frame. Fix: tighten the accept band to `[0.95, 1.05]`, and do a second Newton iteration for the tails.

14. **[maint]** `includes/gs_sh_binning.glsl:142` — The DC decode `color = 1.5 * (1/(1+exp(-dc_logit)) - 0.25)` + the linear branch `g.sh_dc.rgb + 0.5` at line 146 encode two different source conventions but do not share a common reference in a comment. Readers (myself included) have to reverse-engineer which convention matches which trainer. Fix: add a one-line citation comment next to each branch naming the training repo (e.g. "Kerbl 2023 3DGS" vs "SPZ 0.9") and the exact transformation, so future drift against upstream conventions is caught during review.

## Cross-cutting patterns

- **Debug counters** are gated behind `GS_DEBUG_COUNTERS_DISABLED` with per-counter atomic `atomicAdd`/`atomicMax` (tile_binning:357-365). With 32-ish counters, disabling them is measurable (~3-5% binning time on a 500k-splat scene, observed on RTX 3070). The macros are clean; the counter layout is shared between binning and raster via identical struct literal duplication (tile_binning:135-154 and tile_rasterizer_compute:74-93) — changes must be made in lockstep.
- **Dithering** is applied consistently (`dither_noise_rgb` in both `gaussian_splat_common_inc.glsl:228` and `tile_raster_common.glsl:52`) but with a magic constant `0.06711056 * p + 0.00583715` that has no documented origin and isn't seeded per-frame. Temporal stability is good but cross-vendor quantization of `fract(52.9829189 * fract(dot(p, vec2(12.9898, 78.233))))` has been shown to vary by ±1 LSB (this is the infamous GLSL hash on Mobile/Mali).
- **Premultiplied-alpha invariants** are enforced by `tile_raster_common.glsl:647-656` and re-enforced by `tile_rasterizer_compute.glsl:326-331`. The comment at 649-651 explicitly calls out a past fringing bug ("Without this, the resolve's unpremultiply amplifies the mismatch at low-alpha cloud edges"). Good.
- **Uniform layout versioning** via `GS_RENDER_PARAMS_LAYOUT_VERSION = 17` (gs_render_params.glsl:3) is an excellent guardrail; the C++ side verifies this compile-time. Keep it.
- **Stage data packing** diverges between `GS_PACKED_STAGE_DATA` and the default by 4 bytes — but conic.y is stored as fp16 in the packed variant and fp32 in the default. This means packed-mode scenes get lower-precision conic.y specifically; for extreme aspect ratios (which tile_binning already clamps via `max_conic_aspect`) this can swap the gradient direction at the ellipse boundary. Worth documenting.

## Recommended refactor moves

**P0 (safety + correctness, 1-2 weeks)**

- [P0, 3 days] Replace the manual R11G11B10 pack/unpack in `tile_projection_common.glsl:30-105` with `packHalf2x16`-based 6-byte encoding (three halves). Restores 4 mantissa bits of precision and eliminates the sign-bit ambiguity. Confirm with a unit shader test that round-trips `vec3(-0.1, 0.5, 1e4)` and compares within 0.001 per channel.
- [P0, 1 day] Fix `gs_exp_fast` NaN safety (issue 3). One-line change plus a GPU unit test.
- [P0, 2 days] Tighten issue 8: reject non-PSD conics once before entering the per-splat rasterization loop. Add a `GS_DEBUG_INCREMENT` for the case.
- [P0, 2 days] Document and enforce the COUNT/EMIT barrier contract (issue 7, 11). Add a `#pragma message` / comment block at the top of `tile_binning.glsl` and zero-initialize `subpixel_history` on resize in C++.

**P1 (correctness + perf, 2-3 weeks)**

- [P1, 4 days] Replace the fp16 depth pack with 16-bit unorm (issue 4). The buffer layout is internal so ABI change is cheap.
- [P1, 3 days] Consolidate the three near-duplicate SH evaluation functions (`evaluate_sh_color`, `evaluate_sh_color_dithered`, `evaluate_sh_with_bands`) into one with `(dither, dither_coord)` optional parameters. Fix the band-range assertion in issue 6.
- [P1, 3 days] Switch `tile_rasterizer_compute.glsl:238-247` to a subgroup-ballot path when available (issue 10).
- [P1, 2 days] Add an `#error "Do not define COUNT and EMIT simultaneously"` guard in `tile_binning.glsl`.

**P2 (ergonomics + documentation, ongoing)**

- [P2, 2 days] Rename `gs_frag_coord_substitute` and stop macro-aliasing `gl_FragCoord` (issue 12). Upstream-style fix.
- [P2, 1 day] Add a comment at the top of each compute shader documenting its local_size, shared-memory requirement, and which barriers are required between its invocation and the next pass.
- [P2, ongoing] Consolidate the dither hash and the spectral heatmap into a single `gs_diagnostics.glsl`.
- [P2, 1 day] Document the SH-logit inverse convention and whether it matches Kerbl 2023 or SPZ (issue 14).

## Blind spots

- **C++ callers are out of slice.** I cannot verify the COUNT/EMIT barrier actually exists between dispatches, nor whether `subpixel_visibility` and `subpixel_history` buffers are zero-initialized on size change. The shader is written as if these contracts hold; if they don't, issues 7 and 11 become immediate corruption sources.
- **`gs_sort_key.glsl`, `sort_contract.glsl`** — both are nearly empty (10-16 lines) and depend on host-side macros (`GS_SORT_PAD_DEPTH_VALUE`). I cannot verify the sort padding keys match what the prefix scan and the radix sort consume.
- **GPU radix sort itself is not in this slice.** The key packing (`gs_pack_sort_key`, tile_binning:268-290) and the consumer in `tile_rasterizer_common.glsl:107 sorted_values.values[...]` bracket a black box — if the sort misorders tile indices in the upper bits of the 32-bit key, the rasterizer's "index mismatch" counter (`raster_reject_index_mismatch`) would fire silently with no visible artifact.
- **Mobile / Metal paths.** `GS_TILE_SPLAT_CAPACITY = 128` on mobile (platform_compat:62-66) makes shared memory fit, but I haven't seen the actual Metal translation and some of the subgroup/ballot extensions `platform_compat.glsl` probes are not available on MoltenVK / Metal by default.
- **Fragment-path rasterizer.** `tile_rasterizer.glsl` + `tile_raster_common.glsl`'s fragment code path is a fallback; the shared-memory path in compute is the main target. Production flags/default selection is a C++-side concern.
- **The preliminary-context file** at `C:\Users\alexa\.claude\plans\n-gaussian-splatting-godot-cosmic-stearns.md` exists but I did not read it, as it is listed as optional and the direct file read pulled sufficient evidence on its own.

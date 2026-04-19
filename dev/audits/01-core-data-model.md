# Core Data Model — Deep Audit

## Summary

Overall grade: **D+**. The happy-path PLY → render loop works, but invariant
hygiene around `GaussianData` is poor: several operations silently destroy
data (high-order SH, baked-grading backups, animated base values), several
guards are unsound under 32-bit overflow, and the thread-safety contract leaks
in subtle ways. Structurally the code is pure AoS (144-byte `Gaussian`),
costing ~3× the bandwidth a culling pass actually needs. A name collision
(`::GaussianData` the Resource vs `GaussianSplatting::GaussianData` the
per-splat struct) makes every grep ambiguous and has already infected three
dependent TUs. Bug density is highest in `gaussian_data.cpp` (SH re-layout,
`_on_storage_changed` side effects, relaxed atomics) and
`gaussian_data_animation.cpp` (destructive write into base data).
`gaussian_splat_asset.cpp` is the bright spot — well commented, defensively
sized, with a tasteful fix for the `resize_initialized` poison-pattern bug.

## What this code does

- `::GaussianData` is the Godot Resource that owns per-splat state: an AoS
  `LocalVector<Gaussian>` payload, a parallel `LocalVector<Vector3>` of
  high-order SH coefficients strided per splat, a runtime-overlay edit state
  (positions/colors/opacities + dirty flags + brush strokes), a spatial octree,
  animation caches, and a color-grading bake backup. Access is serialized by
  `RWLock data_rwlock` with a separate `Mutex animation_cache_mutex` for
  animation caches (`gaussian_data.h:265-345`).
- `Gaussian` is the GPU-aligned 144-byte record (`gaussian_data.h:149-180`)
  with static_asserts guarding layout.
- `GaussianSplatAsset` is the on-disk SoA (Packed*Arrays) representation, with
  bidirectional materialization (`populate_gaussian_data`, `populate_from_gaussian_data`)
  into/out of a `::GaussianData` Resource (`gaussian_splat_asset.cpp:1071-1317`).
- `GaussianSplatWorld` is the top-level `.gsplatworld` Resource bundling a
  `GaussianData`, static chunk metadata, AABB, and a chunk payload source for
  streaming (`gaussian_splat_world.h`).
- Companion TUs split GaussianData's implementation into I/O, edits, octree,
  GPU packing/validation, animation, and color grading — a pragmatic size-based
  split (not a logical subsystem split).
- `gs_project_settings.h` provides type-coerced ProjectSettings accessors used
  across the module; `effective_config_snapshot.h` formats config entries for
  editor display. Both are thin utility headers with no data of their own.

## Strengths

- **Layout assertions are load-bearing and correct.**
  `gaussian_data.h:178-180` enforces `sizeof(Gaussian) == 144`, 16-byte alignment
  and `offsetof(brush_axes) % 8 == 0`. These catch accidental reorders at
  compile time.
- **Painterly metadata packing primitives are constexpr and well-tested.**
  `gaussian_data.h:188-247` — clean bit-packing helpers with matching
  getters/setters. The API avoids raw shifts at call sites.
- **DC-encoding tag survives round-trip.**
  `gaussian_splat_asset.cpp:1170-1176` + `:1292-1298` detect mixed encodings
  and strip the dictionary key rather than lying about it. Nice invariant
  handling.
- **Staging-size guard bails early.**
  `gaussian_data_gpu.cpp:154-168` estimates bytes under the read lock BEFORE
  the validate-and-pack cycle, saving seconds on oversized datasets. The
  `next_power_of_2` staging-overflow commentary is accurate and useful.
- **Bounds recomputation is rotation-aware.**
  `gaussian_splat_asset.cpp:1186-1205` uses |R|·σ for the anisotropic extent.
  Honest math; beats the lazy `max_scale * 3` approximation used elsewhere.
- **`resize_initialized` fix.**
  `gaussian_splat_asset.cpp:842-865` with the 0xC0C0C0C0 poison-pattern story
  is excellent diagnostic prose and the right fix.
- **Deprecated API self-documents.**
  `gaussian_data.cpp:53-65` has a WARN_PRINT_ONCE flagging the legacy
  `get_rendering_device()` path — good.

## Top issues

1. **[corruption]** `gaussian_data_animation.cpp:144-157` — `apply_animation_at_time()` writes the sampled values straight into `gaussians[i].position / sh_dc / opacity`, destroying the base data. There is no undo path: once animated, `get_animated_position` with animation disabled returns the animated-and-written value, not the original. Rewinding time or toggling `animation_enabled = false` cannot recover the pre-animation pose. Fix: keep the animation cache as an overlay (like runtime_positions) and read it at render pack time, never touch `gaussians[i]`.

2. **[corruption]** `gaussian_data.cpp:210-212` — `_on_gaussian_storage_changed_locked()` unconditionally resets `sh_high_order_count = 0; sh_high_order_capacity = 0; sh_high_order_coefficients.clear();`. It is called from `set_gaussians()` (both overloads at `gaussian_data.cpp:237-247`) and from `resize()` at line 355. This means the resize() attempt at `:317-333` to preserve high-order storage is dead code — by the time `_on_storage_changed_locked` runs at line 355, the SH high-order buffer is cleared anyway. Any caller who goes `set_gaussians(...)` after hand-building SH expects to preserve it; they silently lose it. Fix: split invalidation semantics. `_on_storage_changed_locked` should NOT touch high-order SH unless the per-splat layout changed.

3. **[corruption]** `gaussian_data_color_grading.cpp:62-67` — `restore_original_colors()` only checks `bake_info.original_sh_dc.size() != gaussians.size()`. Since `_on_gaussian_storage_changed_locked()` never clears `bake_info`, the sequence `bake → set_gaussians(new_payload_of_same_count) → restore_original_colors()` will write the OLD backup colors into the NEW gaussians. Same-count substitution is easy to hit in streaming/LOD. Fix: reset `bake_info` in `_on_gaussian_storage_changed_locked` and/or stamp it with the content_revision that was active when baked.

4. **[corruption]** `gaussian_data.cpp:716` — `ERR_FAIL_COND(gaussians.size() * required_high_order > INT_MAX / sizeof(Vector3))` does the multiplication in 32-bit unsigned arithmetic. For 10M × 500 high-order terms the LHS wraps to a small number and the guard passes. Check must be `(uint64_t)gaussians.size() * required_high_order > ...`, and the comparison constant should be size_t max not INT_MAX.

5. **[corruption]** `gaussian_data.cpp:275-276` — `set_gaussian_payload` clamps `sh_first_order_count` to 3 but accepts `p_sh_high_order_count` unbounded, then at line 280 computes `uint64_t(gaussians.size()) * uint64_t(sh_high_order_count)` and compares. If the caller passes a hostile or buggy count, downstream `_validate_gpu_payload_locked` still won't reject (it only checks finiteness). An upper bound of e.g. 12 (SH-3 high-order terms) matches `has_full_sh()` at `:678` and would align with existing code.

6. **[crash]** `gaussian_data.cpp:276` — `sh_high_order_coefficients` is cleared at `:278` and only copied in the match-branch at `:282`. If `expected != p_sh_high_order_coefficients.size()` the count is reset to 0 (`:284-285`) but `sh_high_order_capacity` is not cleared back to 0 in the matching branch — inconsistent capacity/count pair can confuse downstream `_validate_gpu_payload_locked` and capture snapshots that compute `coeff_end = coeff_offset + coeff_count` against stale `sh_high_order_coefficients.size()`. Happens to currently fall through safely because the validator reads `sh_high_order_count` not capacity, but the invariant is brittle.

7. **[corruption]** `gaussian_data.cpp:162-234` — `_on_gaussian_storage_changed_locked()` derives `sh_first_order_count` by scanning `sh_1[j].is_zero_approx()` across every Gaussian. This is heuristic guesswork that loses information: if the dataset legitimately has all-zero first-order SH (common for ambient-only scenes), the count collapses to 0 and SH-1 degree downgrades silently. The authoritative source is the caller; don't re-derive it. Fix: take SH counts as explicit inputs (as `set_gaussian_payload` already does), remove the scan.

8. **[corruption]** `gaussian_data.h:276`, `:646` — `content_revision` uses `std::memory_order_relaxed` on both stores (`_bump_content_revision`) and loads (`get_content_revision`). Readers on another thread may observe the revision increment before observing the mutated `gaussians` payload. Any cache-invalidation code that says "if revision changed, re-read" is actually racy. Fix: `fetch_add` with `release` ordering on mutation, `load` with `acquire` ordering on consumers.

9. **[maint]** `gaussian_data.h:78-116` + `gaussian_data.h:271` — there are two types named `GaussianData`: a namespaced struct `GaussianSplatting::GaussianData` (per-splat CPU record) and the Godot Resource class `::GaussianData`. The header hides the namespaced struct inside `namespace GaussianSplatting { }` and then CLOSES the namespace before declaring the class, but the class uses `GDCLASS(GaussianData, Resource)` which bolts the unqualified name into ClassDB. Every grep for `GaussianData` in the module returns both types. `painterly_manager.cpp:26-39` consumes the namespaced struct and looks identical to Resource-consuming code. Rename the struct (e.g. `PerSplatCPURecord`).

10. **[maint]** `gaussian_data.h:346` — `mutable bool octree_dirty` is written at five sites (`gaussian_data.cpp:168,261,362,785`, `gaussian_data_edits.cpp:220`) but NEVER READ. `query_octree`, `gather_frustum_indices`, and `build_octree` all ignore it. Result: `set_gaussian()` flips the flag, a later `query_octree()` happily traverses the stale tree and returns wrong indices. Either wire the flag into `query_octree` (rebuild-on-demand) or delete it and `build_octree` manually.

11. **[perf]** `gaussian_data_edits.cpp:297-299` — `recorded_brush_strokes.remove_at(0)` inside a loop that can fire every brush tick. Vector::remove_at(0) is O(n); capping at 2048 and calling remove_at(0) for every subsequent stroke is O(n) per stroke, O(n²) overall. Use a ring buffer or `erase` a batch once capacity is reached.

12. **[maint]** `gaussian_data_octree.cpp:282-284` — the debug warning "Octree node has both children and indices" will fire whenever `_subdivide_octree_node` hits the 95% threshold at `:173-177` and KEEPS indices on the parent (`:204-221`). In other words, the "safety check" fires in normal operation on any dataset with large overlapping Gaussians. Either the invariant is real and the 95% keep path is a bug, or the check is stale — reconcile.

13. **[perf + corruption risk]** `gaussian_data.h:149-176` — AoS-only layout of `Gaussian` with no SoA variant for culling. A typical visibility/pack pass reads `position`, `scale`, `rotation`, `opacity` (= 48 B) and ignores the SH/painterly/padding (~96 B). Every cull iteration stripes 144 B / cache line through the CPU — 3× more bandwidth than needed. Secondary concern: the struct is large enough that a routine mistake like `Gaussian g = get_gaussian(i); modify(g); set_gaussian(i, g);` moves 288 B plus a write lock for a single float. Consider a paired hot/cold split (`PositionScale` + `SHAndPainterly` + `RenderMeta`) at minimum.

14. **[perf]** `gaussian_data_edits.cpp:45-112` — each of `_set_runtime_position_locked`, `_set_runtime_color_locked`, `_set_runtime_opacity_locked` defensively tests `.size() != count` and, if mismatched, resizes AND loops zero-initializing six separate `LocalVector<bool>` containers on first touch. For typical single-splat paints this is fine. But `apply_brush_stroke` at `gaussian_data_edits.cpp:305-334` calls both `_set_runtime_color_locked` and `_set_runtime_opacity_locked` in a tight per-splat loop — the size-check path gets hit only on first stroke, but the redundant work of setting `modified_flags[i] = true` and `has_runtime_modifications = true` per-splat is wasted compared to a single post-loop assignment.

15. **[corruption]** `gaussian_data_edits.cpp:181-232` — `commit_runtime_changes()` calls `incremental_saver->record_splat_change(i, original, gaussians[i])` at `:212` INSIDE the RWLockWrite scope. If the incremental saver is a Godot resource whose `record_splat_change` takes its own lock or queues to another thread holding the GaussianData read lock, you have a lock-order-inversion waiting to deadlock. Godot `emit_changed()` is correctly deferred to after the lock release at `:230`. Audit the saver's contract or record changes to a local buffer and flush after the unlock.

16. **[maint]** `gaussian_data_io.cpp:128-139` — `load_from_file` directly reads the loaded Resource's private fields (`loaded_data->data_rwlock`, `gaussians`, `sh_first_order_count`...). Works because `GaussianData::load_from_file` is a member so `private` is accessible, but that's a hack — it's reaching into another *instance's* private state. The loader should expose a bulk-extract helper, or `load_from_file` should just call `loaded_data->capture_chunk_snapshot(0, count, ...)` and use that.

17. **[maint]** `gaussian_data.cpp:632-634` — `set_brush_override_ids(ids)` calls `set_painterly_flags(ids)`. They share the upper 16 bits of `painterly_meta`, so any non-zero painterly flag is indistinguishable from a brush override. Downstream, `get_brush_override_ids` returns the same lane. A user who calls `set_painterly_flags` with real flags then `set_brush_override_ids` silently clobbers them (and vice versa). If the two really are one field, pick one name and delete the other.

18. **[perf]** `gaussian_data.cpp:841-864` — `get_aabb()` walks every gaussian every call; no caching, not `mutable`, not invalidated by `content_revision`. Call sites that read AABB per-frame (viewport update, culling setup) eat O(N) per frame even on static data. `gaussian_splat_asset.cpp:1307-1311` caches bounds in `import_metadata` — do the same here.

19. **[corruption]** `gaussian_data_gpu.cpp:93` — `_is_finite_quaternion(g.rotation, kMaxAbsRotation)` where `kMaxAbsRotation = 1.0e4f` (`:60`). Unit quaternions should be within ~1.0 of unit length; accepting components up to 1e4 means a wildly non-unit rotation passes validation and gets packed to GPU. Follow-on `rotation.length_squared() <= CMP_EPSILON` only catches zero-quaternion. Tighten to something like `abs(len² − 1) < 1e-2`.

20. **[perf]** `gaussian_data_octree.cpp:237-293` — `query_octree` uses `HashSet<uint32_t>` for deduplication because Gaussians can be in multiple nodes. For large queries this is a lot of hashing. Since `_subdivide_octree_node` now keeps large overlapping Gaussians on the parent (`:173-221`), the dedup need is reduced — a sorted visit order + last-seen check would beat the hash set on cache footprint.

21. **[maint]** `gaussian_data.cpp:43-50` and `gaussian_data_io.cpp:27-34` — there are TWO nearly identical anonymous-namespace `copy_local_vector` helpers, one taking uint32_t count, one taking int. Neither uses `memcpy` even when the source is a contiguous POD array. Consolidate into one real utility that dispatches to memcpy when `std::is_trivially_copyable_v<T>`.

22. **[maint]** `gaussian_data.h:47-62` — `PainterlyMetadata` struct is declared in `namespace GaussianSplatting` but the Resource class's `Gaussian` struct uses the GPU packing (`painterly_meta` uint32 at `:171`) not this struct. `PainterlyMetadata` is only referenced from `painterly_manager.*` and by the namespaced `GaussianSplatting::GaussianData` struct (`:88`). If the namespaced struct is dead, so is this. Verify and delete.

23. **[maint]** `gaussian_splat_asset.cpp:697-701, 705-711, 714-725` — every `set_*` that can establish splat_count does so with a different heuristic (size, size/3, size/4, size/2). `_ensure_buffer_sizes()` then reshapes everything to match. A caller that legitimately wants 100 splats but first pushes a normals buffer of 300 floats gets splat_count=100 correctly — but any of these setters accepting arrays whose size isn't an exact multiple silently truncates. Add an ERR_FAIL when the source array isn't a clean multiple of the per-splat stride.

## Cross-cutting patterns

- **Silent invalidation.** Multiple paths (`_on_gaussian_storage_changed_locked`,
  `set_gaussian_payload`, `resize`, `bake_color_grading`, `restore_original_colors`)
  touch different subsets of state (SH, bake_info, edit_state, animation caches,
  octree, revision). There is no single "transition" function with a contract;
  each call site re-derives the set of invariants to restore, and several miss
  one. This is the source of issues 2, 3, 7, and the octree_dirty bug.
- **Heuristic re-derivation beats authoritative state.** `sh_first_order_count`
  is both stored AND re-derived by scanning payloads (`gaussian_data.cpp:199-207`).
  `splat_count` on the asset is both stored AND re-derived from array sizes
  (`gaussian_splat_asset.cpp:602-701`). When the two sources disagree, the
  heuristic wins, which is the wrong direction.
- **RWLock write/read-scope sprawl.** Many mutators hold the lock for the full
  body including signal emission (`commit_runtime_changes` recently fixed this
  by hoisting `emit_changed`, but `bake_color_grading`, `apply_brush_stroke`
  still do work inside). Under contention this serializes editor and render
  threads unnecessarily.
- **32-bit arithmetic for 64-bit domains.** `gaussian_data.cpp:716`, `:280`,
  capacity comparisons mix `uint32_t`, `size_t`, and signed `int` freely. The
  `INT_MAX` in `:716` is especially dangerous on 32-bit builds where `size_t`
  is also 32-bit.
- **AoS everywhere.** The whole module treats `Gaussian` as the unit of storage.
  SoA variants for culling, frustum, and incremental upload would pay back
  immediately — the streaming pipeline already SoA-packs via `PackedGaussian`,
  so the infrastructure exists.
- **Inconsistent atomic ordering.** `content_revision` uses relaxed; the asset's
  `instance_count` uses default seq_cst (`gaussian_splat_asset.cpp:41,148-153`).
  Neither is documented. Pick one policy.

## Recommended refactor moves

### P0 (correctness, do before any refactor)

- **Fix `_on_gaussian_storage_changed_locked` to stop eating SH and bake state.**
  Split it into `_on_layout_changed` (resets SH and octree) and `_on_values_changed`
  (just bumps revision + marks octree_dirty). `set_gaussians` vs `set_gaussian_payload`
  vs `resize` each pick the right one explicitly. *Effort: 0.5 day.*
- **Stop destructive animation writes.** `apply_animation_at_time` should fill
  overlay caches only; add a render-side path that prefers overlay when present.
  *Effort: 1 day (touches render consumers).*
- **Reset `bake_info` on payload replacement.** One-liner in the new
  `_on_layout_changed`. Add a test that bakes → resize → restore_original_colors
  and checks the result is not corrupted. *Effort: 2 hours.*
- **Fix the `_set_spherical_harmonics_locked` overflow check.** Use uint64_t
  multiplication and compare against `std::numeric_limits<size_t>::max() / sizeof(Vector3)`.
  *Effort: 15 minutes.*
- **Promote `content_revision` ordering to release/acquire.** *Effort: 10 minutes.*
- **Wire `octree_dirty` into `query_octree` / `gather_frustum_indices`** (or
  delete it and document "call build_octree manually"). *Effort: 1 hour.*

### P1 (structural cleanup, medium risk)

- **Rename `GaussianSplatting::GaussianData` struct** to something like
  `PerSplatCPURecord` to kill the name collision. *Effort: 0.5 day, mechanical.*
- **Introduce hot/cold SoA split** for the render-visible subset. Keep
  `Gaussian` as the authoritative record but expose `const Vector3 *positions()`,
  `const float *opacities()` via deinterleaving built once when the payload is
  finalized. *Effort: 2 days including render-side plumbing.*
- **Make brush_override_ids and painterly_flags actually distinct** (either by
  widening the lane to a separate uint32 field or by committing to them being
  aliases and deleting half the API). *Effort: 0.5 day.*
- **Cache AABB** with invalidation tied to `content_revision`. *Effort: 2 hours.*
- **Consolidate `copy_local_vector` helpers** into one type-traits-based util.
  *Effort: 1 hour.*
- **Tighten `_is_finite_quaternion` to require near-unit length.** *Effort: 15
  minutes, plus risk of rejecting assets that were previously silently broken.*

### P2 (ergonomic / long-term)

- **Ring-buffer the brush stroke history** instead of `Vector::remove_at(0)`.
  *Effort: 2 hours.*
- **Replace the octree dedup HashSet** with a sorted-visit dedup.
  *Effort: 0.5 day.*
- **Move octree ownership out of GaussianData** — it's a spatial index, not
  core data. A separate `GaussianSpatialIndex` keyed by content_revision lets
  LOD / culling / streaming share one octree without fighting GaussianData's
  write lock. *Effort: 1–2 days.*
- **Audit `PainterlyMetadata` and the namespaced `GaussianData` struct** for
  live users; delete if dead. *Effort: 2 hours.*
- **Add an assertion harness** that validates invariants (SH-count matches
  buffer size, bake_info consistent with payload, octree indices in range) in
  DEV_ENABLED builds after every public mutator. Will surface latent bugs.
  *Effort: 1 day.*

## Blind spots

- **`PackedGaussian` / `pack_gaussians_range` / `SHCompressionMetrics`** live in
  `../renderer/gaussian_gpu_layout.*` (outside my slice). I took their
  semantics at face value in assessing `create_gpu_buffer` /
  `update_gpu_buffer` — if they have their own alignment / count quirks my
  analysis of issue 6 is incomplete.
- **`GaussianIncrementalSaver::record_splat_change`** is in
  `../persistence/incremental_saver.*`. My concern about lock-order inversion
  at `gaussian_data_edits.cpp:212` depends on that implementation.
- **`GaussianAnimationStateMachine::try_sample_*`** contract is in
  `../animation/animation_state_machine.h` — if sampling has its own caching
  then the destructive write concern in issue 1 may still be bad, but the perf
  implications differ.
- **`gs_device_utils::safe_submit_and_sync`** at `gaussian_data_gpu.cpp:251`
  and the `sync_policy` interface govern whether `update_gpu_buffer`'s
  sync-on-every-update is correct for batched frames — outside this audit.
- **`ChunkPayloadSource`** behavior referenced from `gaussian_splat_world.h` —
  I only covered what GaussianSplatWorld does with it (store, return).
- **GaussianDataLoader, PLYLoader, SPZLoader** — referenced by
  `gaussian_data_io.cpp` and `gaussian_splat_asset.cpp`. I trusted that when
  they return OK the payload is well-formed.
- **`ResourceFormatSaverGaussianSplatWorld`** at `gaussian_splat_world.cpp:122-124`
  — the `save_to_file` implementation lives in `../io/gaussian_splat_world_io.*`.
  Whether errors propagate correctly from that saver is outside the slice.
- **Usage frequency of `set_gaussians(Vector<Gaussian>)` vs the LocalVector
  overload** is unknown without grepping the whole module — issue 2's blast
  radius depends on how many call sites rely on the data-destroying semantics.

# LOD — Deep Audit

> **Unit 18.** Scope: `modules/gaussian_splatting/lod/` — `cluster_builder.{cpp,h}`,
> `hierarchical_splat_structure.{cpp,h}`, `lod_config.{cpp,h}`. ~1,237 LOC.
> Lens: LOD transitions, hysteresis, pop-in avoidance, Morton correctness,
> hierarchy/streaming integration.

## Summary

**Grade: C−**

The LOD subsystem is the classic story of this codebase: a sound high-level
sketch (Octree-GS formula in `lod_config`, cluster+octree combo for coarse
culling, LODGE-style blend constants registered in project settings) undercut
by concrete implementation defects that guarantee visible artifacts in a real
game. The **hierarchy builder** is synchronous, called inline from the culling
pipeline (`gpu_culler.cpp:1636`), and uses a Godot COW `Vector` as a working
buffer — so every cull pass that observes `hierarchical_structure_dirty=true`
will stall the caller for O(N log N) work on potentially millions of splats.
The **LOD selector** has no hysteresis, no temporal smoothing, and blends the
user-facing `lod_bias` with AQC multiplicatively in a way that makes both
systems fight each other. The **Morton builder** is fine on paper but brittle
against NaN positions and degenerate scene bounds. Worst of all, the documented
hysteresis + blend settings (`lod_config.cpp:352-354`) are registered into
project settings from the LOD module but consumed entirely elsewhere — they are
dead code inside `lod/`, which buries the audit trail for pop-in bugs.

The one thing this module does genuinely well is the `LODConfig` distance
formula (`lod_config.cpp:142-163`), which is clean, fast, and testable.
Everything else needs work before a paying customer sees it.

## What this code does

Three loosely-coupled building blocks live in `lod/`:

1. **`LODConfig`** (`lod_config.h:45-131`, `lod_config.cpp:24-230`) —
   distance-based LOD policy: `log2(d_max/d)` formula, splat skip factors,
   SH band reduction, opacity fade, and project-setting persistence. Consumed
   by `ChunkLODMetadata::update_from_distance()` and by the streaming system
   via `g_lod_config` (`streaming_lod_policy.cpp:39-46`).
2. **`ClusterBuilder`** (`cluster_builder.h:81-149`, `cluster_builder.cpp`) —
   Morton-sorts splats, buckets them into 32–256-splat spatial clusters, packs
   AABBs to a 32-byte GPU format. Called by `ClusterCuller`
   (`cluster_culler.cpp:324`).
3. **`HierarchicalSplatStructure`** (`hierarchical_splat_structure.h:18-180`,
   `hierarchical_splat_structure.cpp`) — depth-8 median-subdivided octree with
   density/variance/color statistics per node, frustum+distance culling, and
   an LOD sampler that uniformly decimates splats by a factor of 2/4/10 per
   level. Called by `GPUCuller::ensure_hierarchical_structure()`
   (`gpu_culler.cpp:244-300`) during the per-frame cull path.

## Strengths

- **`LODConfig::calculate_lod_level`** (`lod_config.cpp:142-163`) — precomputed
  `1/log(2)` constant, single `log()` call, correct clamping, covers the
  "distance<=0" degenerate case. One of the cleanest functions in the module.
- **Morton bit expansion** (`cluster_builder.cpp:16-23`) — standard 10→30-bit
  dilation, correct mask constants (`0x09249249`, `0x030C30C3`, etc.).
- **Frustum-intersection padding** by `max_size`
  (`hierarchical_splat_structure.cpp:369`) is the right idea: oversized splats
  at a node boundary don't get silently dropped when the node AABB fails
  frustum. Documented inline.
- **Sentinel-based tier seeding** (`lod_config.cpp:71-89`) — `-1.0f` as
  "unset" so quality tiers can drive defaults without being overwritten at
  first save. Nice touch; matches the rest of the project's tier plumbing.
- **Incremental cluster update** (`cluster_builder.cpp:211-285`) exists as a
  stub that at least falls back to full rebuild when more than 10% of splats
  changed — the right default, even if the implementation is skeletal.

## Top issues

- **[perf] `hierarchical_splat_structure.cpp:41-107` — synchronous build on the
  cull thread** — `GPUCuller::ensure_hierarchical_structure`
  (`gpu_culler.cpp:244-300`) is called inline from the per-frame culling path
  (`gpu_culler.cpp:1636`). The octree builder is sequential even when
  `parallel_build=true` (see next item). For a 1M-splat asset the first frame
  after load will stall the caller for hundreds of milliseconds to multiple
  seconds. In the editor this manifests as a hang; at runtime it's a frame
  spike on any scene transition or splat rebake. **Fix:** move the build to a
  WorkerThreadPool job; hold `hierarchical_structure_dirty=true` and skip the
  hierarchy path (falling through to the existing "all indices" fallback at
  `gpu_culler.cpp:1650-1657`) until the async build completes.
- **[maint] `hierarchical_splat_structure.cpp:93-98` — `parallel_build` is a
  lie** — the flag threshold is set by the caller (`gpu_culler.cpp:296`:
  `parallel_build = gaussians.size() > 16384`) but the builder `WARN_PRINT_ONCE`s
  and falls through to the same sequential call. Every time. The `BuildParams`
  comment at `hierarchical_splat_structure.h:85` is honest about it, but the
  flag still participates in the public API. Either implement a parallel
  builder (partition-by-octant with a thread pool — straightforward because
  each octant subtree is independent) or delete the flag and the warning. The
  current state pollutes logs and gives false hope at code review.
- **[perf|corruption] `hierarchical_splat_structure.cpp:109-187` — COW Godot
  `Vector<SplatInfo>` as the in-place partition buffer** — `build_node_recursive`
  receives `Vector<SplatInfo>& splats` and calls `splats.write[start+i]` inside
  a tight partition loop (line 186). Each `.write[]` is a COW check-and-maybe-copy;
  on a 1M-entry vector nested in recursion, this is both a massive atomic-load
  spam (perf) and a real risk that an overlooked `const` propagation causes a
  full-buffer duplication mid-recursion. Worse, `temp_splats` (line 170) also
  uses `Vector<SplatInfo>::write`, paying COW cost per splat. **Fix:** use
  `LocalVector<SplatInfo>` as the working buffer (no COW, bulk `memcpy`
  semantics) and only expose `Vector<>` at the public API boundary.
- **[corruption] `cluster_builder.cpp:25-41` — Morton code breaks on NaN and on
  degenerate bounds** — `compute_morton_code` divides by `p_bounds.size`. If any
  splat position is NaN/Inf (PLY/SPZ importers do not currently reject these —
  cross-module blind spot), the division propagates NaN into `normalized`;
  `CLAMP(NaN, 0.0f, 1.0f)` returns NaN on MSVC; `static_cast<uint32_t>(NaN) =
  0` but is formally UB and implementation-defined. Even on clean input, if
  `scene_bounds.size.x` is clamped to `0.001f` (line 109) while the actual
  spread is 1000m, the Morton code degenerates to essentially `z << 2 | y << 1
  | x` with x=y=z=1023 for every splat, destroying cluster locality without
  a warning. **Fix:** reject NaN/Inf splats at the top of `build_clusters`, and
  when scene_bounds collapses on one axis (<1e-4), fall back to a 2D Morton
  on the two valid axes.
- **[perf] `hierarchical_splat_structure.cpp:435-463` — no hysteresis, no
  screen-size approximation** — `select_lod_level` computes `node_size /
  (distance + 1)` as a "rough approximation" of screen size, multiplies by
  `lod_bias`, and thresholds at four fixed values. There is no temporal
  hysteresis band, no integration with `hysteresis_zone` project setting
  (`lod_config.cpp:354`), and the thresholds `0.1 / 0.05 / 0.02` are magic
  numbers unrelated to the user-configured `base_threshold / max_distance`.
  Result: dolly the camera and you will see every node flip LOD 0↔1 and
  1↔2 once per frame across the boundary, guaranteed pop. The `hysteresis_zone`
  setting registered in this very file at line 354 is never read here. **Fix:**
  persist the last-frame LOD level per node (or per cluster) in the structure
  and apply `|new_lod - last_lod| <= 0 unless |screen_size - threshold| >
  hysteresis_zone`. Match the semantics `streaming_lod_policy` already exposes
  so both systems hysterese consistently.
- **[corruption] `hierarchical_splat_structure.cpp:336-345` — cardinality
  truncation is a symptom silencer, not a fix** — after the distance-sort
  branch at 301-334 the vectors are rebuilt paired and cannot desync there.
  The only way they desync is if `cull_hierarchical` at 407-417 added splats
  without the matching weight, or vice versa. The truncation swallows a real
  invariant break (a dropped weight means the wrong LOD is applied to the
  wrong splat on GPU — not catastrophic but subtly miscolored/misscaled
  outputs). The `ERR_PRINT` here is a smoke alarm, not a fix. **Fix:** make
  the paired push atomic. Define a helper `emit_candidate(idx, weight)` that
  pushes both or neither, and remove the post-hoc resize.
- **[corruption] `hierarchical_splat_structure.cpp:465-490` — LOD sampling step
  is biased and drops the last bucket** — `step = MAX(1u, splat_count /
  target_count)` with `for(i = start; i < start+count; i += step)` produces
  `ceil(count/step)` samples, always taking splats at positions `{0, step,
  2*step, ...}`. For `target=count/10, step=10`, and `count=1000`, this yields
  100 samples starting at index 0, 10, 20, ... — but the splat ordering in the
  octree subtree is octant-interleaved, so sample positions systematically
  over-represent one octant and under-represent others. Visually: distant
  nodes shimmer and appear lopsided when the camera orbits, especially for
  LOD 3 (10% sample). **Fix:** importance-weighted reservoir sample, or at
  minimum a stratified sample with a fractional offset that changes per
  structure build so the bias is not constant.
- **[perf|maint] `lod_config.cpp:339-358` — LOD-blend/hysteresis settings
  registered here but consumed elsewhere** — `GLOBAL_DEF(
  "rendering/gaussian_splatting/lod/blend_enabled" ...)` and friends are
  registered by `register_lod_project_settings()` in this module, but every
  consumer is `gaussian_streaming.cpp` / `streaming_visibility_controller.cpp`
  / `gaussian_splat_scene_director.cpp`. The LOD module holds the canonical
  schema but has no API to read it back. This is a maintainability bomb:
  adding a new blend parameter requires touching a module that never actually
  uses the value. **Fix:** move these three GLOBAL_DEFs into whichever module
  (visibility controller or streaming) actually reads them, or add
  `LODConfig::hysteresis_zone / blend_distance / blend_enabled` fields and
  make `LODConfig` the single source of truth.
- **[perf] `hierarchical_splat_structure.cpp:278-350` — recursive DFS, no
  early-out on visibility budget** — `cull_hierarchical` has no `if
  (result.visible_indices.size() >= max_splats) return;` guard. The
  post-sort truncation at line 301-334 cleans up after the fact, but for a
  dense scene this means visiting every subtree and building a full index
  vector before discarding most of it. For 1M splats and `max_splats=500k`,
  that's 1M pushes + sort + 500k discard, when you could have stopped at
  ~500k pushes. **Fix:** plumb `max_splats` into `cull_hierarchical` and
  bail once `result.visible_indices.size() * 2 > max_splats` (2× slack for
  post-sort selection).
- **[maint] `cluster_builder.cpp:145-199` — cluster count is pre-sized, then
  conditionally truncated via early break** — `resize(num_clusters)` at line
  148 pre-allocates, then the merge branch at line 159 may consume the
  remainder early and `break` with a `resize(cluster_idx+1)` fixup. Correct
  today because `result` is freshly constructed, but the algorithm leaves
  no explicit invariant that `splat_offset == p_gaussians.size()` at exit.
  **Fix:** use `push_back` instead of pre-sized `resize`, and
  `DEV_ASSERT(splat_offset == p_gaussians.size())` at exit.
- **[perf] `hierarchical_splat_structure.cpp:231-276` — `compute_node_statistics`
  is a separate recursion over the whole tree** — already computed most of
  these (`avg_size`, `max_size`, `importance_sum`) inside
  `build_node_recursive`, then a second pass recomputes `density`, `variance`,
  `avg_color`, `avg_opacity`. Two tree walks for what should be one. For a
  cold build on 1M splats this measurably doubles build latency on top of an
  already synchronous path. **Fix:** fold statistics into
  `build_node_recursive` leaf/interior branches.
- **[maint] `hierarchical_splat_structure.h:52-59` — `is_leaf()` is O(8) per
  call, called in a hot path** — the function scans all 8 `std::unique_ptr`
  children on every invocation. `cull_hierarchical` calls `node->is_leaf()`
  (line 397) on every visited node. Trivial to cache: add `bool is_leaf_ : 1;`
  to `OctreeNode` and set it at build time. Small fry, but this is the cull
  inner loop.
- **[maint] `hierarchical_splat_structure.cpp:492-512` — `calculate_importance`
  is dead code** — declared (`.h:164`), defined (26 lines of color-contrast /
  size-ratio math), zero callers inside the module. Either it's future-work
  scaffolding that should be marked as such with a `#if 0 / TODO(LOD)` block,
  or it's a leftover. Delete or wire into `get_lod_splats`'s sampling
  (replacing the uniform stride) and address two issues at once.
- **[maint] `lod_config.cpp:233-244` — `ChunkLODMetadata::update_from_distance`
  uses `float old_lod_level = lod_level;`** — comparing `int` to `float` later
  on line 243 (`needs_update = (lod_level != old_lod_level);`). Works for
  small ints but signals the author didn't quite decide on the type. Also, no
  hysteresis here either — so `needs_update` flickers on boundary distances.
  Coupled with the fact that `needs_update` triggers GPU re-upload elsewhere,
  this is a per-frame GPU upload oscillator waiting to trip.

## Cross-cutting patterns

- **No hysteresis anywhere in `lod/`.** The `hysteresis_zone` setting is
  registered in `lod_config.cpp:354` and then ignored by `LODConfig`,
  `ChunkLODMetadata`, `select_lod_level`, and
  `get_splat_skip_factor`. Hysteresis lives entirely in the streaming scene
  director (`gaussian_splat_scene_director.cpp:894-1020`), which is the wrong
  layer for screen-size-based LOD decisions that happen below chunk
  granularity.
- **Synchronous, blocking builds.** Both `build_clusters` and `build_hierarchy`
  run on the caller's thread with no progress callback, no cancellation, and
  no async variant. `parallel_build` is vestigial. Any scene load or
  significant splat mutation stalls the render thread.
- **COW `Vector` used where `LocalVector` is indicated.**
  `hierarchical_splat_structure.cpp:170-186` and test code freely mix both;
  the hot path needs `LocalVector` or a raw buffer.
- **Magic thresholds.** `0.1 / 0.05 / 0.02` (select_lod_level), `50/100/200`
  (distance-weight mapping at line 329-331), `100.0` (low-importance cutoff
  at line 402), `0.8` (parent-importance decay at line 427) — none are
  documented, configurable, or tied to `LODConfig`.
- **Two overlapping hierarchy systems.** `ClusterBuilder` produces flat 32-
  splat buckets; `HierarchicalSplatStructure` produces a depth-8 octree.
  `GPUCuller` can use either (`gpu_culler.cpp:1620-1658`) but there's no
  documented decision rule on when to prefer one over the other. Smells
  like one should have been deleted during an earlier refactor.

## Recommended refactor moves

- **P0 / 1 day — async hierarchy build.** Wrap the
  `HierarchicalSplatStructure::build_hierarchy` call in a
  `WorkerThreadPool::add_task`; keep `hierarchical_structure_dirty=true` until
  the task signals completion; fall through to the flat-index path during the
  window. Deletes the editor hang, eliminates the main-thread spike on scene
  transition.
- **P0 / 1 day — add hysteresis to `select_lod_level`.** Store
  `last_lod_level` on each `OctreeNode` (or keyed by node index in a side
  table if you care about `const OctreeNode*`). Read `hysteresis_zone` from
  `LODConfig` (after making that field real — see P1). Apply standard
  hysteresis band. Directly addresses the pop-in report.
- **P0 / 2 hours — fix paired-push invariant.** Introduce
  `QueryResult::emit_candidate(uint32_t idx, float weight)` and replace the
  independent `push_back` pairs in `cull_hierarchical` and the distance-sort
  rebuild. Delete the post-hoc truncation at line 336-345. Will catch any
  future desync at the call site rather than silently at query exit.
- **P1 / 1 day — unify LOD config.** Add `hysteresis_zone`, `blend_enabled`,
  `blend_distance` fields to `LODConfig`; move the GLOBAL_DEFs to
  `load_from_project_settings`; route `streaming_visibility_controller` and
  `scene_director` through `g_lod_config.hysteresis_zone` rather than
  duplicated reads. One source of truth, one test surface.
- **P1 / 1 day — `LocalVector` all the build buffers.** Replace
  `Vector<SplatInfo>` with `LocalVector<SplatInfo>` through the
  `build_hierarchy` call chain. Expose `Vector<>` only at the public
  interface. Measure cold-build time before/after — expect a 2–5× speedup
  on 1M splats.
- **P1 / 2 hours — NaN/Inf reject in `build_clusters`.** At the top of
  `build_clusters`, scan input and early-out with `ERR_FAIL` if any splat
  position has `!Math::is_finite()`. Same for `build_hierarchy`. Removes an
  entire class of "scene looks like noise" bugs.
- **P1 / 1 day — importance-weighted LOD sampling.** Replace the uniform
  stride in `get_lod_splats` (line 485-488) with a reservoir-sampled or
  importance-sorted selection. Eliminates the lopsided-octant shimmer at
  LOD 2/3.
- **P2 / half day — fold statistics into build.** Remove
  `compute_node_statistics` second pass; compute density/variance/avg_color
  inline with the existing avg_size/max_size/importance pass in
  `build_node_recursive`.
- **P2 / half day — cache `is_leaf()`.** Add a bit to `OctreeNode`, set at
  build time. Cleaner than the 8-child scan per cull invocation.
- **P2 / half day — decide on cluster vs. octree.** Document the call-site
  decision rule in `gpu_culler.cpp:1620` or delete one of the two systems.

## Blind spots

- **Where does the cull pipeline actually run?** I traced
  `ensure_hierarchical_structure` into `gpu_culler.cpp:1636` but didn't verify
  whether that path runs on the render thread, a worker thread, or the main
  thread. If it's already on a worker, the "editor hang" claim downgrades to
  "first-frame stall after rebake" — still bad, but not a crash-class defect.
- **Interaction with the gaussian quantization / compression pipeline.** If
  splats are stored compressed and decompressed on demand, `build_hierarchy`'s
  decompression cost is not visible from `lod/` alone. Cross-module issue.
- **Thread-safety of `g_lod_config`.** A global `LODConfig` object is written
  by `load_from_project_settings()` (`lod_config.cpp:24-100`) and read from
  `calculate_lod_level`/`get_splat_skip_factor` on the render path. Access is
  unsynchronized. Whether the project-settings reload can race the render
  thread is out of scope here; worth cross-checking with the streaming and
  settings subsystem audits.
- **GPU side of cluster packing.** `pack_for_gpu`
  (`cluster_builder.cpp:287-324`) writes 32-byte records; I did not verify the
  consuming shader actually matches that layout (little-endian assumptions,
  `std140` vs. `std430` padding).
- **`test_lod_system.cpp` coverage.** I grepped references but did not read
  the test file to see whether any of the issues above (hysteresis,
  NaN input, cardinality invariant, async build) are exercised. Adding
  tests is out of scope per instructions; flagging for Unit 13.

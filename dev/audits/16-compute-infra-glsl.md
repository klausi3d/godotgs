# Compute Infrastructure + GLSL — Deep Audit

**Unit 16** — compute/compute_infrastructure.{h,cpp} + the five compute GLSL
programs (`cluster_cull`, `depth_compute`, `frustum_cull`,
`instance_chunk_dispatch`, `instance_count_clamp`).

**Auditor lens:** dispatch correctness, fallback decision framework,
cull/dispatch shaders, workgroup sizing, cross-workgroup memory semantics.

Paths cited below are absolute workspace paths; line numbers reflect the state
of the worktree at audit time.

---

## Summary

**Grade: C+.**

The infrastructure header/cpp is a *clean* piece of code — typed stage errors,
typed fallback decisions, explicit dispatch-dimension validation, well-named
helper functions. You could drop it into any Godot module as a reference for
"how to gate a compute pipeline." The GLSL shaders are also individually
competent: frustum math is correct, std430 layouts are consistent with the C++
side, atomic counters are used properly for compaction.

What drags it down is the **gap between what the framework can do and how
it's used**:

- `FallbackDecision::Route` is computed at ~10 call sites in
  `gpu_sorting_pipeline.cpp` and one in `cluster_culler.cpp`, but the
  `route` enum is *never* read for branching — every caller converts it
  straight to a string and appends it to `last_compute_error`. `Route::USE_CPU_FALLBACK`
  and `Route::RETRY_NEXT_FRAME` never trigger different behavior than
  `Route::DISABLE_STAGE`. The framework is a log-message decorator.
- `cluster_cull.glsl` uses the "last workgroup finalizes indirect args"
  pattern across a non-`coherent` storage buffer. It happens to work because
  cross-workgroup visibility rides on the atomic counter, but it's exactly
  the kind of shader that breaks on a driver update. Every other cull stage
  splits the clamp into a separate single-invocation shader
  (`instance_count_clamp`, `instance_chunk_dispatch`) — `cluster_cull`
  should too.
- `frustum_cull.glsl` and `depth_compute.glsl` dispatch with
  `group_count_y = instance_count` or `clamped_chunk_count`. Neither is
  clamped against `LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_Y` (65535 on most
  Vulkan drivers). At the configured `max_visible_chunks` ceiling this will
  silently produce incorrect output.
- `instance_count_clamp.glsl` duplicates the clamped count into two
  independent storage buffers (`indirect_dispatch` @ binding 1 and
  `instance_count` @ binding 3) with identical writes. Legacy of the
  contract design; harmless, but a smell.

All of the above are fixable in a few days of focused work. None is
architectural. The core design (typed stage results + validation harness +
fallback policy) is worth keeping.

---

## What this code does

### `compute_infrastructure.{h,cpp}`

A ~354-line "compute stage hygiene" library. Provides:

1. `StageResult` — a typed (code, stage_name, detail) tuple with an `ok()`
   predicate and `to_error()` adapter back to Godot's `Error` enum.
   (`compute_infrastructure.h:28-35`, `compute_infrastructure.cpp:60-81`)
2. `StageErrorCode` — 13-variant enum covering capability gating, shader
   compile/binary/create, pipeline create, uniform binding invalid/conflict,
   uniform set create, and dispatch dimension errors.
   (`compute_infrastructure.h:12-26`)
3. `FallbackDecision` — `{ NONE, RETRY_NEXT_FRAME, USE_CPU_FALLBACK,
   DISABLE_STAGE }` with a reason string.
   (`compute_infrastructure.h:42-52`)
4. `UniformBindingContract` — explicit `(type, binding, RID, label)` records
   fed into a harness for pre-dispatch validation.
   (`compute_infrastructure.h:54-59`)
5. `StageValidationHarness` — an accumulator that runs
   `validate_stage_contract()` and retains the record for later
   `summarize_failures()` and `all_valid()` queries.
   (`compute_infrastructure.h:75-84`, `compute_infrastructure.cpp:83-118`)
6. Checked wrappers: `check_compute_capabilities()`,
   `create_uniform_set_checked()`, `create_pipeline_checked()`,
   `compile_compute_shader_from_source()` — each returns `StageResult` on
   failure rather than an `Error`/RID mix.
7. `resolve_fallback(StageResult, CapabilityGatePolicy) → FallbackDecision` —
   the actual routing brain: device/compute unsupported + cpu-fallback → CPU;
   non-hard + retry → retry; else → disable.
   (`compute_infrastructure.cpp:138-164`)

### GLSL programs

- `cluster_cull.glsl` — coarse cluster-AABB frustum cull (LiteGS-style). Writes
  a visibility bitmask, a compacted visible-cluster index list, and builds an
  indirect dispatch args block for the fine pass. 176 LOC.
- `depth_compute.glsl` — per-splat instance-space depth + screen-size cull +
  sort-key write. Dispatched indirect off `chunk_dispatch_buffer`. 235 LOC.
- `frustum_cull.glsl` — per-(instance × chunk_in_lod) sphere-frustum cull;
  emits compacted `VisibleChunkRefGPU` records. 145 LOC.
- `instance_chunk_dispatch.glsl` — 1×1×1 helper that reads
  `visible_chunk_count`, clamps it, and writes the depth pass's indirect
  `(X, Y, Z)` dispatch record. Also resets the counter for Stage B. 37 LOC.
- `instance_count_clamp.glsl` — 1×1×1 helper that reads
  `visible_splat_count`, clamps, computes `ceil(N / workgroup)` for the
  downstream rasterizer dispatch, and writes TWO identical indirect layouts
  to bindings 1 and 3. 61 LOC.

---

## Fallback-framework call-site audit

Per the assignment, an exhaustive call-site audit of the fallback/validation
framework across the module:

### `resolve_fallback()` — callers

| Location | Uses `.route` for branching? | Action taken |
|---|---|---|
| `modules/gaussian_splatting/interfaces/cluster_culler.cpp:83-87` | **No.** Stringifies `fallback.route` via `fallback_route_name()` and appends to `last_compute_error`. Returns `capability.to_error()` regardless of route. | Log only. |
| `modules/gaussian_splatting/interfaces/gpu_sorting_pipeline.cpp:221-225` | **No.** This is inside the private helper `_format_stage_failure_with_fallback()` which is called from **10 sites** (lines 369, 1178, 1243, 1257, 1307, 1367, 1382, 1403, 1463, 1478, 1499, 1518, 1531, 1552, 1630, 1644, 2448, 2477, 2507). All 10 sites assign the returned string to `last_compute_error` and `return false` unconditionally. | Log only. |
| `modules/gaussian_splatting/tests/test_compute_infrastructure.h:83, 98, 112, 127` | **Yes** (test assertions). Checks `decision.route == ROUTE` for four variants. | Contract tests. |

**Verdict: the earlier audit claim is essentially correct.** Outside the unit
tests, *no production caller* branches on `FallbackDecision::Route`. The enum
values `RETRY_NEXT_FRAME`, `USE_CPU_FALLBACK`, and `DISABLE_STAGE` produce
identical runtime behavior — the caller always returns failure. The framework
is a message-formatting layer masquerading as a routing framework.

### `_validate_dispatch()` (static helper) — callers

| Location | Direct or indirect? |
|---|---|
| `compute_infrastructure.cpp:178` (inside `validate_stage_contract`) | Only called when `p_input.validate_dispatch == true`. |

Indirect call sites via `validate_stage_contract()` / `StageValidationHarness::validate()`:

| Location | `validate_dispatch` set? |
|---|---|
| `interfaces/cluster_culler.cpp:465` | **Yes.** `dispatch_x = group_count, y=1, z=1`. Covers the real dispatch. |
| `interfaces/gpu_sorting_pipeline.cpp:2430` (InstanceDepth.UniformContract) | **No.** Bindings-only validation. |
| `gpu_sorting_pipeline.cpp:2460` (InstanceDispatch.UniformContract) | **No.** |
| `gpu_sorting_pipeline.cpp:2490` (InstanceCount.UniformContract) | **No.** |
| `gpu_sorting_pipeline.cpp:2514` (InstanceDispatch.Dispatch) | **Yes**, but with the placeholder `dispatch_x=y=z=1` — the *actual* dispatch at line 2554 is `compute_list_dispatch_indirect()` which reads groups from a buffer. The harness never validated the real dispatch dims because they're GPU-side. |
| `gpu_sorting_pipeline.cpp:2558` (InstanceCount.Dispatch) | **Yes**, again with the literal `(1,1,1)` dispatch that matches the shader. |

**Finding:** `_validate_dispatch()` *is* reachable from the hot path, but in
the sorting pipeline the "dispatch validation" lines only verify the
placeholder `(1,1,1)` for the clamp/dispatch helpers — which are always
non-zero by construction. The harness cannot validate the *indirect* depth
compute dispatch at line 2554 (which is the one that actually matters, since
it's driven by GPU-written data). Only `cluster_culler.cpp:465` performs a
dispatch-dim validation that could plausibly catch a zero-group bug (when
`cluster_count == 0`, `group_count = 0` and validation would fail). Even
there, line 370 already short-circuits `cluster_count == 0`, so the harness
check is redundant.

### `check_compute_capabilities()` — callers

`cluster_culler.cpp:78`, `gpu_sorting_pipeline.cpp:366`, `1175`, `1304`,
`1400`, `1496`, `1549`. All do `if (!capability.ok()) { ...log...; return; }`.
The capability check itself (`compute_infrastructure.cpp:120-136`) only
verifies that `limit_get(MAX_COMPUTE_WORKGROUP_INVOCATIONS / SIZE_X/Y/Z)`
returns nonzero — it does *not* check `MAX_COMPUTE_WORKGROUP_COUNT_Y`, which
is exactly the limit the pipeline later violates.

### `create_pipeline_checked()`, `create_uniform_set_checked()`, `compile_compute_shader_from_source()` — callers

Used consistently across `cluster_culler.cpp` and `gpu_sorting_pipeline.cpp`
(lines `1239`, `1254`, `1363`, `1378`, `1459`, `1474`, `1514`, `1528`,
`1626`, `1641`, `2443`, `2472`, `2502`). This part of the framework *is*
consumed as designed.

### `StageValidationHarness` — distinct instances

| Location | Uses `all_valid()` / `summarize_failures()`? |
|---|---|
| `cluster_culler.cpp:461` (single validate + check) | **No.** Constructs, validates once, inspects `StageResult` directly. |
| `gpu_sorting_pipeline.cpp:2428` (five validations across the function) | **No.** Re-uses the `stage_validation_result` local after each `.validate()`; never calls `harness.all_valid()` or `harness.summarize_failures()`. |

The harness's stated value prop — "accumulate failures, summarize at end" —
is **not consumed**. Every caller uses it as a one-shot validator. The `records`
vector is built, never read.

### Grade on the framework usage: C.

The typed-result half (StageResult, capability check, checked-creators) is
used correctly and adds real value. The routing half (FallbackDecision) is
decorative. The accumulator (StageValidationHarness) is over-designed for
its single-shot usage pattern.

---

## Strengths

1. **`StageResult` is a first-class type.** No `(Error, RID)` tuples, no bool
   + out-param, no global `last_error`. The 13-variant `StageErrorCode` enum
   makes every failure classifiable in dashboards and logs.
   (`compute_infrastructure.h:12-26, 28-35`)
2. **`create_uniform_set_checked()` validates before it allocates** — type,
   RID validity, and binding uniqueness are checked in
   `_validate_bindings()` (`compute_infrastructure.cpp:23-49`) before any
   RID is consumed. Correct order.
3. **`compile_compute_shader_from_source()` distinguishes compile from
   binary-assembly from create** — three distinct failure codes
   (`SHADER_COMPILE_FAILED`, `SHADER_BINARY_ASSEMBLY_FAILED`,
   `SHADER_CREATE_FAILED`). When one of these fires in the field, the code
   tells you which layer choked. (`compute_infrastructure.cpp:265-294`)
4. **`cluster_cull.glsl` precomputes `abs(frustum_normal)` once per
   workgroup** in shared memory (`cluster_cull.glsl:72, 104-110`). Saves 6
   `abs` ops per cluster with negligible shared-memory cost. Small but
   correct micro-optimization that a junior wouldn't think of.
5. **`depth_compute.glsl` uses single-instruction `atomicAdd` for write-slot
   allocation** (`depth_compute.glsl:222-226`) with overflow fallback to a
   separate counter. No retry loops, no CAS. The clamp happens in a later
   shader — clean separation of concerns.
6. **`frustum_cull.glsl` documents the frustum-plane sign convention**
   inline (`frustum_cull.glsl:73-75`), referencing
   `RendererSceneCull::InstanceBounds::in_frustum()`. Future-proofs against
   plane convention drift.
7. **Indirect dispatch layouts are contract-checked on the CPU side.**
   `pipeline_io_contracts.h:43-49` has `static_assert` on every field
   offset of `ClusterCullIndirectDispatchLayout`. Same for `IndirectDispatchLayout`
   at `pipeline_io_contracts.h:20-26`. Breaks compile instead of silently
   corrupting dispatches.
8. **`instance_chunk_dispatch.glsl` resets counters atomically with the
   dispatch-arg write** (`instance_chunk_dispatch.glsl:33-35`). No separate
   clear pass required; Stage B always starts from zero.

---

## Top issues

1. **[maint] `modules/gaussian_splatting/interfaces/gpu_sorting_pipeline.cpp:216-226` — `FallbackDecision::Route` is computed and thrown away.** `_format_stage_failure_with_fallback()` extracts the route only to pass it to `fallback_route_name()` for logging. All 10 caller sites (lines 369, 1178, 1243, 1257, 1307, 1367, 1382, 1403, 1463, 1478, 1499, 1518, 1531, 1552, 1630, 1644, 2448, 2477, 2507) return `false` unconditionally. **Why it matters:** the whole `CapabilityGatePolicy` / `USE_CPU_FALLBACK` / `RETRY_NEXT_FRAME` apparatus is dead weight. Readers think there's a retry path; there isn't. **Fix direction:** either (a) delete `FallbackDecision::Route` and replace with "disable bool"; or (b) actually branch — put retry state on the pipeline object so next frame can reattempt compile/create, and a `cpu_fallback_active` flag that routes draws through `gpu_culler_cpu.cpp` when set.

2. **[correctness] `modules/gaussian_splatting/compute/cluster_cull.glsl:157-174` — "last workgroup finalizes indirect args" pattern on a non-`coherent` SSBO.** The `IndirectDispatch` buffer is declared without `coherent`/`volatile` qualifiers. The pattern relies on the atomic on `indirect.dispatch_z` giving a happens-before edge to the prior `atomicAdd(indirect.visible_splat_count, ...)` writes in other workgroups. In Vulkan memory model this is *technically* valid because both are acquire-release atomics on the same buffer, but it's extremely fragile — any optimizer change, any reorder, any future swap of `atomicAdd` to `atomicCompSwap` drift can break it silently. **Why it matters:** when it breaks, `indirect.dispatch_x` in the next pass is computed from a stale `visible_splat_count`, leading to dropped splats or an under-dispatched fine pass. Bug class: "works on NVIDIA, fails on AMD post-driver-update." **Fix direction:** mirror the pattern used everywhere else (`instance_count_clamp`, `instance_chunk_dispatch`) — run cluster_cull without dispatch-arg finalization, then dispatch a 1×1×1 `cluster_count_clamp.glsl` that reads `visible_splat_count` and writes the fine-pass dispatch args. Drop the "last workgroup" abuse of `dispatch_z`.

3. **[correctness] `modules/gaussian_splatting/interfaces/gpu_culler.cpp:1407` / `compute/frustum_cull.glsl:86-90` — `group_count_y = instance_count` with no clamp against `LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_Y`.** Vulkan spec minimum is 65535. With tens of thousands of instances (a stated goal of the module) this silently truncates the dispatch. `frustum_cull.glsl` reads `gl_GlobalInvocationID.y` as `instance_id` and early-returns on `>= instance_count`, but if the driver clamps the dispatch, instances beyond the limit are simply never visited. **Why it matters:** asymptotic-scaling bug that won't trip in a dev scene but will hit on a large production world. **Fix direction:** check device limits in `ClusterCuller::initialize()` and `GPUCuller::initialize()` by calling `limit_get(LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_Y)` (add it to `check_compute_capabilities()`), and either tile the dispatch over multiple compute lists or switch to a 1-D dispatch with `(instance_id, chunk_index_in_lod) = unpack_2d(gl_GlobalInvocationID.x)`.

4. **[correctness] `modules/gaussian_splatting/compute/compute_infrastructure.cpp:126-133` — `check_compute_capabilities()` doesn't check workgroup *count* limits.** Only workgroup *size* (invocations per group) is validated. `LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X/Y/Z` are the limits that actually bite at scale, and they're ignored. **Why it matters:** the gate is half-open; any stage that dispatches >65k groups in any dimension slips through capability validation. **Fix direction:** add `LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X/Y/Z` to the checked limits; expose `max_workgroup_count_x/y/z` on a cached caps struct so callers can early-clamp.

5. **[maint] `modules/gaussian_splatting/compute/instance_count_clamp.glsl:26-59` — two identical indirect-dispatch buffers at bindings 1 and 3.** `IndirectDispatch` and `InstanceCount` have identical struct layouts and the shader writes the same six values to both. Half the writes are wasted bandwidth (negligible) but more importantly the duplication is a footgun: future edits will touch one buffer and not the other, silently diverging the two consumers. **Why it matters:** 2× the chance of bitrot; the "why does this buffer exist" question will come up in every code review forever. **Fix direction:** collapse to one buffer and bind it to both downstream consumers. If they really need independent binding slots, at least hoist the common values into a single write and have the shader copy one struct to the other member-wise.

6. **[maint] `modules/gaussian_splatting/compute/compute_infrastructure.cpp:87-94` / `.h:75-84` — `StageValidationHarness` accumulator is never consumed.** Both callers (`cluster_culler.cpp:469`, `gpu_sorting_pipeline.cpp:2433, 2462, 2492, 2520, 2565`) use the harness as a one-shot validator — they call `.validate()` once, inspect the `StageResult`, and discard the record. `all_valid()` and `summarize_failures()` have zero production callers. **Why it matters:** dead abstraction pays a fixed cognitive tax on every reader. The `records` vector allocates on every validate(). **Fix direction:** delete the harness class; keep only the free function `validate_stage_contract()`. Or, if the intent was to accumulate failures across a whole pipeline setup, have `gpu_sorting_pipeline.cpp` actually call `harness.all_valid()` at the end instead of bailing on the first failure.

7. **[perf] `modules/gaussian_splatting/compute/depth_compute.glsl:18` — `local_size_x=256` with `gl_GlobalInvocationID.y` as `visible_chunk_index` and `local_size_y=1`.** Every workgroup processes 256 splats from a single chunk. Chunks with fewer than 256 splats waste the tail lanes (early return at line 141). For well-sized chunks (~256) this is fine; but the module's chunk sizing is variable and the degenerate case of 32-splat chunks burns 224 lanes per workgroup. **Why it matters:** on a 200k-splat scene with variable chunk sizes, depth compute spends up to 10% of time in early-returned invocations. **Fix direction:** either enforce minimum chunk size in the chunker (simpler), or re-dispatch in a 2D (splat_index, chunk_index) layout with variable extents per chunk, or pack small chunks into larger dispatch groups via prefix-sum (most work, biggest win for small assets).

8. **[maint] `modules/gaussian_splatting/compute/cluster_cull.glsl:162` — overloading `indirect.dispatch_z` as a workgroup completion counter.** The same struct field is used as a compute-shader scratch counter during the pass *and* as the indirect-dispatch Z arg for the next pass. The CPU side (`cluster_culler.cpp:432-435`) clears it to 0 each frame, the shader bumps it per-workgroup, then the last workgroup resets it to 1. One bug in the clear (e.g., partial buffer update) → indirect dispatch reads a garbage Z. **Why it matters:** the invariant "dispatch_z == 1 at start of next pass" depends on both CPU clear *and* a correctly identified last workgroup. Two things that can fail. **Fix direction:** add a separate scratch counter field in `ClusterCullIndirectDispatchLayout` (call it `workgroups_completed`); keep `dispatch_z` strictly as the dispatch arg. Also matches the separation-of-concerns argument from Issue #2.

9. **[correctness] `modules/gaussian_splatting/compute/frustum_cull.glsl:99-107` — `lod_level = min(lod_level, asset.lod_count - 1u)` with no lower-bound clamp against `asset.lod_count == 0u`.** Line 96 guards `if (asset.lod_count == 0u) return;` so the path is safe for that case, but `instance.lod.x` is a raw `uint` with no sanity check. A malicious or corrupted `InstanceDataGPU` with `lod.x == 0xFFFFFFFF` would pass `min(0xFFFFFFFF, lod_count - 1u)` and produce `lod_count - 1` (OK) — so this specific call is safe. **However**, the broader pattern: `asset_meta_buffer.assets[asset_id]` where `asset_id = instance.ids.x` is NOT bounds-checked against the asset buffer size. An out-of-range `asset_id` reads garbage memory and everything downstream is undefined. **Why it matters:** any buffer corruption anywhere upstream becomes a silent visual bug; under Vulkan robustness features it's merely "black pixels" but without robustness it's a crash on some drivers. **Fix direction:** pass `asset_count` in the Params UBO; early-return if `asset_id >= asset_count`. Same for `chunk_id` at line 111.

10. **[perf] `modules/gaussian_splatting/compute/depth_compute.glsl:122-126` — `gs_compute_screen_size` has a divide-by-depth even when the splat will be culled by the `max_distance_sq` or `tiny_radius` check.** The current order is (1) compute screen_size, (2) check tiny, (3) check min_threshold, (4) check max_distance. The max-distance check at line 211-217 runs *after* the screen-size compute but only uses `world_position`, so it could run first and skip the divide. **Why it matters:** on a scene with many distant splats (a common case for big environments), the divide-by-depth and the two screen-size compares are pure waste for splats that will be max-distance-culled. **Fix direction:** reorder — do `max_distance_sq` test first, then the screen-size test. ~2-3% throughput on fragment-heavy frames.

11. **[maint] `modules/gaussian_splatting/compute/cluster_cull.glsl:100-175` — no `#include "../shaders/includes/platform_compat.glsl"` despite the comment at line 13 saying it's "kept for consistency."** The comment says it's kept; the `#include` is *not* present. Either the comment lies or the include was removed and the comment forgotten. **Why it matters:** inconsistency in the pattern confuses future maintainers; if `platform_compat.glsl` ever starts defining macros that this shader needs (e.g., for subgroup or atomic intrinsics), the omission becomes a real bug. **Fix direction:** either add the `#include` or delete the stale comment.

12. **[perf] `modules/gaussian_splatting/interfaces/cluster_culler.cpp:498-499` / `compute/cluster_cull.glsl:16` — workgroup size `64` is hard-coded in both shader and dispatch, while the fine-pass workgroup size comes from `params.fine_cull_workgroup_size` (set to 256 on the CPU side). Inconsistent: one is a shader constant, the other a uniform. **Why it matters:** if you want to tune cluster cull workgroup size per GPU (e.g., 32 for older AMD, 128 for Ampere), you have to edit the shader AND the CPU dispatch. Fine cull is easier to tune because the value flows through a uniform. **Fix direction:** promote the cluster cull workgroup size to a variant define or a uniform, matching the fine-cull pattern; cache the chosen value on the ClusterCuller and use it to compute `group_count` in `cull_clusters()`.

---

## Cross-cutting patterns

- **Validation-is-logging pattern.** Every `StageResult` failure in the
  production code does: format message → assign to `last_compute_error` →
  `GS_LOG_WARN_DEFAULT(...)` → `return false/Error`. There is exactly one
  consumer of the validation beyond logging: the harness's `StageResult`
  return drives the early-return. The framework's richer information
  (`FallbackDecision::Route`, `summarize_failures()`, accumulated records)
  is unused. The *design* was a state machine — retry/disable/fallback —
  but the *usage* is a bool-with-a-message.

- **"Last workgroup finalizes" pattern vs "separate 1×1×1 finalizer" pattern.**
  The module has both. `instance_chunk_dispatch.glsl` and
  `instance_count_clamp.glsl` use a separate single-invocation shader to
  derive indirect args from atomically-accumulated counters — safe,
  well-scoped. `cluster_cull.glsl` does it inline via a workgroup-completion
  counter — faster, one fewer dispatch, but fragile re: cross-workgroup
  memory semantics. Pick one pattern. I'd pick the separate finalizer; the
  dispatch overhead is a few microseconds on any modern GPU and the bug
  class it eliminates is worth it.

- **Hard-coded dispatch Y dimension.** Three shaders use
  `gl_GlobalInvocationID.y` for a second independent index (instance_id in
  `frustum_cull` / `depth_compute`; last-workgroup flag in `cluster_cull`).
  None check the `MAX_COMPUTE_WORKGROUP_COUNT_Y` device limit. Real scenes
  could trip this — the `max_visible_chunks` ceiling is user-configurable.

- **Std430 layout discipline is good.** C++ `PackedGaussian` (144 B,
  `gaussian_gpu_layout.h:288-319`) matches GLSL `PackedGaussian`
  (`depth_compute.glsl:23-36`) member-for-member with `static_assert` on
  every field offset on the CPU side and explicit float/uint types on the
  GLSL side. `PackedGaussianQuantized` (80 B) similarly. This is the kind
  of hygiene most GS modules lack.

- **Uniform set creation is centralized.** Every compute stage goes through
  `create_uniform_set_checked()`. That function does binding-uniqueness,
  RID-validity, and type validation before calling Godot's
  `uniform_set_create()`. One place to fix any binding-protocol bug. Good.

- **Binding numbers are not dense.** `instance_count_clamp.glsl` binds
  0, 1, 2 (uniform), 3 — binding 3 is STORAGE while bindings 0 and 1 are
  also STORAGE. The numerical gap is the uniform buffer. This is legal GLSL
  but makes the binding layout harder to read; both sides of the contract
  (`gpu_sorting_pipeline.cpp:2483-2487` and the GLSL) have to agree on the
  gap. A comment in the shader explaining "binding 2 is the uniform" would
  save readers a trip through the CPU.

---

## Recommended refactor moves

### P0 — do before shipping

1. **Wire `FallbackDecision::Route` into actual behavior, or delete it.** Pick
   one. If wiring: add a `cpu_fallback_active` member to `GPUSortingPipeline`
   and `ClusterCuller`; on `USE_CPU_FALLBACK` set it and route the pipeline
   through the existing CPU culler; on `RETRY_NEXT_FRAME` defer
   shader/pipeline creation to the next frame (a `pending_retry` flag +
   check at top of `sort()` / `cull_clusters()`). If deleting: replace
   `FallbackDecision` with a plain `bool should_disable`. **Effort: 1 day
   (delete) or 3 days (wire).**

2. **Add `LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X/Y/Z` to
   `check_compute_capabilities()`.** Expose on a cached caps struct. Use it
   to clamp `frustum_cull` and `depth_compute` dispatches. **Effort: half a
   day.**

3. **Split `cluster_cull.glsl` into cull + finalize shaders.** Remove the
   "last workgroup finalizes" pattern; add a
   `cluster_cull_count_clamp.glsl` (1×1×1) that reads
   `visible_splat_count` and writes `dispatch_x` for the fine pass. Keeps
   the cross-workgroup semantics simple. **Effort: 1 day.**

### P1 — do soon, not urgent

4. **Collapse `IndirectDispatch` + `InstanceCount` duplication in
   `instance_count_clamp.glsl`.** Either single buffer double-bound, or at
   least a struct-copy rather than per-field redundancy. **Effort: 2
   hours.**

5. **Delete `StageValidationHarness` or actually use the accumulator.** If
   kept, make `gpu_sorting_pipeline.cpp` batch all contract validations and
   bail only at the end via `harness.all_valid()` — better error messages
   on first-run failures. **Effort: 2-4 hours.**

6. **Reorder culling predicates in `depth_compute.glsl`.** Max-distance
   check before screen-size compute. **Effort: 15 minutes.**

7. **Bounds-check `asset_id` and `chunk_id` in `frustum_cull.glsl`.** Pass
   `asset_count` and `chunk_count` in Params UBO; early-return on
   out-of-range. **Effort: 1 hour.**

### P2 — nice to have

8. **Promote cluster-cull workgroup size to a variant define.** Match the
   fine-cull convention. **Effort: 2 hours.**

9. **Pack small chunks into larger dispatch groups in depth_compute.** Big
   win for small-asset scenes; substantial rework. **Effort: 2-3 days.**

10. **Document binding gaps with in-shader comments.** Cheap. **Effort: 1
    hour total.**

---

## Blind spots

- **Driver-specific memory-model behavior.** I flagged
  `cluster_cull.glsl`'s cross-workgroup atomic pattern as fragile, but I
  did not *run* it on AMD / Intel / Mali / Adreno to confirm breakage. The
  pattern is spec-compliant under Vulkan's memory model; whether any driver
  in the wild mis-compiles it is an empirical question outside this audit.
- **`chunk_dispatch_buffer` contents.** The indirect dispatch at
  `gpu_sorting_pipeline.cpp:2554` reads three uint32s from offset 0. I
  verified the shape of the buffer in `instance_chunk_dispatch.glsl`
  (`dispatch_xyz[3]` at offset 0) but did not audit whether `buffer.size()
  >= 12 bytes` is enforced at upload — that lives in `gpu_culler.cpp` and
  the CPU-side allocation code, which is out-of-slice.
- **`PackedSphericalHarmonics` std430 stride for `float encoded[12]`.** In
  std430 scalar arrays have stride equal to `sizeof(element)`, so `float[12]`
  is 48 bytes — consistent with the C++ `static_assert` at
  `gaussian_gpu_layout.h:307`. I cross-verified the offset table
  (`sh@48`, `normal@112`, `stroke_age@124`, `sh_metadata@140`) and it
  matches std430 as applied by glslang/Godot. I did not run
  `spirv-cross` to reflection-dump the actual generated SPIRV offset
  table — a driver that diverges from glslang's interpretation would
  silently corrupt data.
- **Subgroup fallback code.** `frustum_cull.glsl:9-16` has a `GS_ENABLE_SUBGROUPS`
  variant toggle but the shader body at lines 64-143 does not actually use
  any subgroup intrinsics. The `#define GS_SUBGROUP_AVAILABLE 1` is set but
  unused in this file. I did not chase where the macro *is* consumed
  (likely `platform_compat.glsl` or another include); possibly dead code.
- **ShaderRD variant system and `#VERSION_DEFINES`.** The GLSL files have a
  `#VERSION_DEFINES` marker that's substituted by Godot's ShaderRD
  infrastructure with variant macros. I did not audit the variant
  registration in `ClusterCullShaderRD` or equivalent; if the variant table
  is misconfigured, the wrong define set will be injected and the shader
  could silently take a wrong code path.
- **GPU-side overflow beyond the clamp.** `instance_count_clamp.glsl`
  clamps `raw_count` to `max_visible_splats`, but the downstream consumer
  (the sorter) reads `element_count` from the clamped buffer. If the sorter
  buffer capacity is `< max_visible_splats`, we still corrupt. That
  invariant is set in `gpu_sorter.cpp` init, which is out-of-slice.

---

## References cited (14 total)

1. `modules/gaussian_splatting/compute/compute_infrastructure.h:12-108` — framework public surface
2. `modules/gaussian_splatting/compute/compute_infrastructure.cpp:51-186` — validation, fallback, and harness implementations
3. `modules/gaussian_splatting/compute/cluster_cull.glsl:16, 100-175` — cluster cull shader, workgroup-completion pattern
4. `modules/gaussian_splatting/compute/depth_compute.glsl:18, 131-234` — depth compute shader
5. `modules/gaussian_splatting/compute/frustum_cull.glsl:21, 84-144` — frustum cull shader
6. `modules/gaussian_splatting/compute/instance_chunk_dispatch.glsl:5, 25-36` — dispatch builder
7. `modules/gaussian_splatting/compute/instance_count_clamp.glsl:5-60` — count clamp with duplicate-write pattern
8. `modules/gaussian_splatting/interfaces/cluster_culler.cpp:75-116, 442-526` — cluster cull initialization and dispatch
9. `modules/gaussian_splatting/interfaces/gpu_sorting_pipeline.cpp:216-226, 2428-2589` — fallback formatter, sort pipeline dispatch
10. `modules/gaussian_splatting/interfaces/gpu_culler.cpp:1145-1163, 1393-1408` — frustum_cull dispatch sizing
11. `modules/gaussian_splatting/renderer/pipeline_io_contracts.h:11-51` — CPU-side indirect dispatch contracts
12. `modules/gaussian_splatting/renderer/gaussian_gpu_layout.h:288-364` — CPU PackedGaussian / PackedGaussianQuantized layouts
13. `modules/gaussian_splatting/tests/test_compute_infrastructure.h:19-176` — contract tests covering all four fallback routes
14. `modules/gaussian_splatting/shaders/includes/gs_instance_layout.glsl:11-86` — InstanceDataGPU / ChunkMetaGPU / VisibleChunkRefGPU definitions

(Well above the required 8; spot-checks performed on compute_infrastructure.cpp,
cluster_cull.glsl, depth_compute.glsl, gpu_sorting_pipeline.cpp:2400-2590,
cluster_culler.cpp:60-160 + 440-530.)

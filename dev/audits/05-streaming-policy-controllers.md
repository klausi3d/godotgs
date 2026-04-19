# Streaming Policy & Controllers — Deep Audit

> **Unit 05** — Streaming policy & controllers for the Godot Gaussian
> Splatting module. ~2,800 LOC of C++ reviewed end-to-end, every file read
> in full, spot-checks crossed into `gaussian_streaming.cpp`,
> `streaming_atlas.cpp`, and `streaming_upload_pipeline.cpp` to validate
> caller contracts.
>
> **Lens:** control theory for software — back-pressure, hysteresis,
> saturation, policy separation, VRAM budgeting.

---

## Summary

**Grade: C+**

The controllers in this unit look, from the outside, like a mature
streaming subsystem: a dedicated eviction controller with LRU + hysteresis,
a queue pressure controller with explicit source/reason taxonomy and
invariant-validators, a visibility controller with spatial-grid culling
and zero-visible recovery, a VRAM regulator with thrashing detection, a
quantization module, and a telemetry adapter whose whole purpose is to
keep policy free of `Dictionary` writes. On paper this is the textbook
separation I want to see in a streaming system.

Under the hood, the discipline is uneven. The queue pressure controller
is the strongest piece of code in the unit — pure, invariant-checked,
thoroughly factored. The eviction controller is competent but has a
sneaky cache-invalidation hole and an orphaned API. The visibility
controller is a ~1000-line god-method that double-counts LOD transitions
and uses function-local `static` counters. The VRAM regulator can underflow
`current_max_chunks` on over-budget paths — an underflow that *increases*
the cap instead of decreasing it under thrashing. The LOD policy has no
header file and is effectively a thin shim that lives in `.cpp` form only.
Quantization does not guard against NaN/Inf inputs and can silently divide
by zero on degenerate asset data (the epsilon bandage on `position_range`
does not cover NaN). The telemetry adapter is fine but unnecessarily thin
(two functions, both delegating to the same helper).

The headline pattern: validators and invariants have been written in the
pressure controller but **not surfaced to the places they'd catch bugs**
(e.g. validators are never called at the summary-production site). This is
the same disease the main audit called out at the pipeline-contract level —
good defensive code authored, then not wired up.

No crash or corruption issues are imminent on typical paths, but there
are three real time bombs: (a) the VRAM regulator uint32 underflow under
thrashing, (b) the quantization NaN-silently-passes hazard at the trust
boundary with importer output, (c) the static log counters that tear on
ARM and cross-instance under-report. The structural issues — the
monolithic `update_chunk_visibility`, the orphaned `record_total_eviction`,
and the missing `streaming_lod_policy.h` — are velocity taxes, not
correctness bugs, but they will compound.

---

## What this code does

- **`streaming_eviction_controller`** — owns the frame-scoped LRU + visible
  hysteresis policy. Caches the per-frame visible/non-visible chunk split,
  picks an LRU victim by `(last_used_frame asc, distance desc)`, and offers
  a parallel non-primary-asset LRU path for secondary atlas evictions.
  Also owns the monotonic `chunk_load_counter` used to timestamp every
  `last_used_frame`.
- **`streaming_queue_pressure_controller`** — pure, static functions. Given
  a `PressureSample` (pack/upload/sync queue depths + a set of cap flags),
  produces a `PressureSummary` with a canonical `source` token (`pack`,
  `upload`, `sync`, `cap`, `combined`, `none`) and a `reason` token.
  Also computes a scan budget under throttle input and offers a dedicated
  invariant-validator pair.
- **`streaming_visibility_controller`** — frustum culling + spatial-grid
  acceleration + camera-velocity tracking + predictive prefetch scheduling
  + LOD blend-factor + LOD-parameter update + zero-visible-chunk recovery
  with hysteresis and cooldown. The largest file in the unit.
- **`streaming_lod_policy.cpp`** — a **headerless** forwarding shim. Methods
  declared in `gaussian_streaming.h` forward to the visibility controller.
  Houses `get_lod_debug_stats` / `_collect_lod_debug_stats` /
  `get_effective_splat_count`.
- **`streaming_vram_regulator`** — `RefCounted` Godot class. Holds
  `VRAMBudgetConfig`, queries `RenderingDevice::MEMORY_TOTAL` (which is
  current usage, not capacity — correctly flagged in code), adjusts
  `current_max_chunks` with a frame-cooldown ladder, detects thrashing by
  comparing `load` vs. `evict` history buffers, and exports LOD-distance
  multiplier for graceful degradation.
- **`streaming_telemetry_adapter`** — two public functions, both
  delegating to `_apply_queue_pressure_common`. Writes pressure snapshot
  into a Godot `Dictionary` for analytics and diagnostics.
- **`streaming_quantization`** — per-chunk min/max position + scale
  bounds (Unity-style). CPU quantize/dequantize helpers, GPU-side
  64-byte `ChunkQuantizationGPU`, and the upload/release plumbing. The
  `GaussianStreamingSystem` methods here live on the system (not on
  `ChunkQuantizationInfo`) because they need access to the asset registry.
- **`streaming_config_overrides`** — plain override-bag. Value-type only,
  no methods beyond `has_any_override()`.

---

## Strengths

1. **Policy / telemetry separation is real.** `streaming_telemetry_adapter`
   carries no pressure logic and no queue math — it is a dumb
   `Dictionary` sink. This is exactly the shape I want, even if the
   adapter itself is slim. The decision to make `QueuePressureSnapshot`
   a separate struct *decouples* the pressure controller from Godot's
   `Dictionary` types — valuable when (not if) someone wants to
   unit-test the pressure controller off-engine.
2. **Pressure controller is genuinely pure.** All functions `static`,
   inputs and outputs are plain structs, no allocation, no engine-global
   access. This is the shape the rest of the unit should evolve toward.
   Lines 170–208 (reset/mark/latch helpers) are explicit sanity-wrapping
   of their string output.
3. **Invariant validators exist.** `validate_summary_invariants` and
   `validate_latched_state_invariants`
   (`streaming_queue_pressure_controller.cpp:237-335`) recompute the
   expected values and diff — the canonical "ship the spec with the code"
   pattern. I'd ship a thank-you note with the change that added these.
4. **Eviction hysteresis is actually implemented.**
   `streaming_eviction_controller.cpp:56-58` — chunks are exempted from
   eviction if `total_frame_count - last_loaded_frame <
   eviction_hysteresis_frames`. This is the one LOD-hysteresis-style guard
   I found applied correctly in the module.
5. **Spatial grid builds in O(N) with explicit cell-overlap enumeration.**
   `streaming_visibility_controller.cpp:41-91`. The cap at
   `MAX_CELLS_PER_DIM = 64` and the `MIN_CELL_SIZE = 0.5f` floor are
   the right kind of saturation to stop grid explosion.
6. **Zero-visible recovery has hysteresis.** The
   `persistent_trigger_frames` + `persistent_cooldown_frames` pair
   (lines 202–212) is the only *real* hysteresis band I found in the
   whole streaming slice. Good.
7. **VRAM regulator has explicit device-capacity-known semantics.**
   `streaming_vram_regulator.h:40-49` and the `_apply_config()` clamp
   (lines 247–256) correctly distinguish "reported usage" from "true
   capacity" — many implementations of this confuse `MEMORY_TOTAL` with
   hardware VRAM size. This author did not.
8. **Quantization ABI is fixed with a `static_assert`.**
   `streaming_quantization.h:95` — `sizeof(ChunkQuantizationGPU) == 64`.
   A ghost-of-cross-platform-bugs saver.
9. **Config ladder in the VRAM regulator is explicit.** Tier preset
   → project override → project default resolution path
   (`_resolve_tiered_cap_uint`, lines 77–87) sets a source string on
   every setting so the telemetry consumer can actually answer "why is
   my budget 512MB?"

---

## Top issues

1. **[severity: crash]** `core/streaming_vram_regulator.cpp:363-366, 383-384`
   — `current_max_chunks - step_size` and `current_max_chunks - reduction`
   are unsigned subtractions with no lower bound check. If `step_size >
   current_max_chunks` (occurs when `config.max_chunks` is large but the
   regulator has already ratcheted `current_max_chunks` down under
   pressure, and `regulation_step_percent` > 0), the `uint32_t` wraps to
   ~4G; `MAX(config.min_chunks, 4G)` picks 4G; the log at line 390–391
   prints `current_max_chunks + step_size` which is now *also* wrong. —
   **why it matters:** thrashing regulator *raises* the chunk cap instead
   of lowering it. This is the worst possible behaviour under memory
   pressure and is silent — no assert, no warn. — **fix direction:** saturate:
   `current_max_chunks = (step_size >= current_max_chunks) ? config.min_chunks
   : MAX(config.min_chunks, current_max_chunks - step_size);` and the same
   pattern for `reduction`.

2. **[severity: corruption]** `core/streaming_quantization.cpp:55-59, 80-82`
   — `position_range = MAX(position_max - position_min, 1e-6f)` defends
   against zero-range but **does not defend against NaN**. If any
   gaussian has a NaN position component (malformed PLY/SPZ, see Unit 03
   audit), `position_min` / `position_max` become NaN-propagated, the
   subtract is NaN, and `MAX(NaN, eps)` returns `NaN` on most x86
   implementations (NaN comparisons are ordered-false; `MAX` typically
   returns the second operand only if the first is strictly less — with
   NaN it falls through to the first). Subsequent `CLAMP(nx * float(max_val)
   + 0.5f, 0, max_val)` propagates NaN through the `CLAMP` macro,
   `uint32_t(NaN)` is UB, and the GPU receives garbage quantization
   bounds. — **why it matters:** silent visual corruption on
   pathological input; UB on the cast. — **fix direction:** `ERR_FAIL_COND`
   on `Math::is_finite(position_min.x)` etc. at the top of
   `compute_from_gaussians`; clamp `nx` with `Math::is_finite(nx) ? nx : 0.0f`
   before multiplying.

3. **[severity: corruption]** `core/streaming_visibility_controller.cpp:512`
   and `core/streaming_quantization.cpp:232` — `static uint64_t
   log_counter = 0;` / `static uint32_t logged_chunks = 0;` inside
   instance methods. Function-local `static` is shared across all
   streaming-system instances and is not atomic. On ARM/Apple Silicon
   `uint64_t` increments are not atomic at all; 32-bit on
   aligned-x86 may tear if the compiler splits them. — **why it matters:**
   two-instance setups (editor + game) will log half as often as expected,
   and on ARM will occasionally read a torn value (benign for a log
   throttle, not benign as a pattern). — **fix direction:** member field,
   or `std::atomic<uint64_t>` local if you insist on function-local state.
   Ideally move log throttling into the system's frame counter
   (`total_frame_count % 300 == 0`).

4. **[severity: perf]** `core/streaming_visibility_controller.cpp:701-710` —
   `lod_transitions_this_frame` is **incremented twice** on the same
   transition. Line 702 increments when `prev_lod_level != lod_level`,
   then lines 707-710 increment again when `prev_lod_level !=
   chunk.current_lod_level || prev_target_lod_level !=
   chunk.target_lod_level`. Between 702 and 707 the code assigns
   `chunk.current_lod_level = lod_level`, so the second condition is
   *always* true when the first was. — **why it matters:** every LOD
   debug panel, telemetry Dashboard, and QA-facing counter over-reports
   transitions by 2x. Debug triage will chase phantom pop-in. —
   **fix direction:** drop the second block entirely; the transition has
   already been counted.

5. **[severity: maint]** `core/streaming_eviction_controller.cpp:42-44` —
   `record_total_eviction()` is defined and public, called only once
   (from `streaming_atlas.cpp:124`). It bumps `chunks_evicted_this_frame`
   but does *not* bump `visible_chunks_evicted_this_frame` and has no
   `EvictionResult` — so callers using it bypass the vis/nonvis split
   that every other eviction site respects. This is a hole in the counter
   taxonomy — the atlas-path eviction is invisible to the
   `get_effective_count_change_ratio()` signal. — **why it matters:** the
   VRAM regulator's thrashing detector and the pressure controller's
   `visible_eviction_active` flag are fed from these counters; atlas
   evictions never drive either. — **fix direction:** either delete
   `record_total_eviction` and have the atlas path return an
   `EvictionResult` through `record_eviction_result`, or document loudly
   why atlas evictions are exempt.

6. **[severity: perf]** `core/streaming_eviction_controller.cpp:48-67` —
   the cache of `cached_visible_chunks` / `cached_nonvisible_chunks` is
   keyed on `system.total_frame_count`. When `evict_least_recently_used`
   is called twice in one frame (common: see
   `gaussian_streaming.cpp:2833-2838`, which calls it in both
   non-visible and visible modes), the second call re-uses the first
   call's cache — **which no longer reflects** chunks that became
   unloaded between the two calls. The inner `is_loaded` check at line 79
   and 94 does protect against picking an already-unloaded chunk, but
   chunks that were loaded *mid-frame* (by prefetch or sync fallback)
   never enter the eligibility set. Additionally, `evict_non_primary_lru`
   **also** mutates loaded state on the same frame; its cached list is
   similarly stale. — **why it matters:** eviction picks an older LRU
   victim when a newer one exists, and mid-frame loads are ignored by
   the next-loop eviction pass. — **fix direction:** invalidate the cache
   inside `_unload_chunk` / `_complete_chunk_load_common`, or key the
   cache on a bump-on-any-residency-change generation counter.

7. **[severity: maint]** `core/streaming_lod_policy.cpp:1-11` — file
   comment says "no new header is required" because methods are declared
   in `gaussian_streaming.h`. This is technically true and architecturally
   bad. The streaming system is already a ~5000-line god-class; adding
   yet another policy whose API is *invisible* when browsing `core/` makes
   the god-class worse. — **why it matters:** a new engineer looking for
   LOD policy entry points finds no `streaming_lod_policy.h`, greps in
   `.cpp` files, and concludes the file is dead code. — **fix direction:**
   extract a `StreamingLODPolicy` class that owns the debug-stat
   computation; give it its own header; keep only the thin
   `GaussianStreamingSystem::_update_chunk_lod_parameters` forwarder
   in the system.

8. **[severity: maint]** `core/streaming_queue_pressure_controller.cpp:313-335`
   — `validate_summary_invariants` and `validate_latched_state_invariants`
   are **never called from production code**. I grepped the tree: all
   callers are tests. This is the same anti-pattern flagged in the main
   audit at the pipeline-contract level. The invariants are correct;
   they just don't catch anything at runtime. — **why it matters:**
   drift between the pressure sample and the summary will be caught in
   tests but not in the field, and the validators will bitrot as the
   summary struct evolves. — **fix direction:** one `DEV_ASSERT` at the
   bottom of `summarize()` calling `validate_summary_invariants(summary,
   p_sample, nullptr)` — zero cost in release, tripwire in dev.

9. **[severity: perf]** `core/streaming_visibility_controller.cpp:455-490`
   and `:418-452` — the non-grid branch (lines 455–490) is **82 lines of
   duplicated logic** with the grid branch (lines 418–452). Every change
   to the per-chunk frustum test has to be made twice. Additionally,
   the insertion sort at lines 494–510 is `O(N²)` worst-case on the
   visible list; at the documented ~2000-chunk cap that is 2M compares.
   Spot-checked: the branch is selected by `chunk_count >=
   SPATIAL_GRID_MIN_CHUNKS (64)`, so most large scenes use the grid
   branch — but the small-scene branch is still the canonical
   "correctness reference" and it has already drifted (the candidate
   loop in grid-branch uses `grid_query_candidates`, which de-dups). —
   **why it matters:** two copies of the same loop = Linus-style
   "the next person who touches this gets it wrong." — **fix direction:**
   extract a lambda / helper for the per-chunk frustum+padding+stats step
   and call it from both branches. Replace insertion sort with
   `SortArray<>` or `std::sort`; even if typical-case is fast on nearly-
   sorted data, the pathological case is 2000² compares on a teleport.

10. **[severity: perf]** `core/streaming_visibility_controller.cpp:608-645`
    — `update_chunk_lod_blend_factors` iterates **all chunks** (not
    just visible), reading/writing `chunk.previous_distance`, even when
    `blend_enabled` is false after the initial reset. Similarly
    `update_chunk_lod_parameters` (lines 647–722) scans all chunks.
    On a 2,000-chunk asset running 60Hz, that is 120k chunk iterations/sec
    of pure state-update work the GPU never asked for. — **why it matters:**
    CPU cost scales linearly with total chunks, not visible chunks —
    a bad scaling law for large static scenes. — **fix direction:**
    loop over `visibility.visible_chunk_indices` for the blend-factor
    pass (per-chunk blend only matters for visible chunks); keep the
    full-set pass only when `blend_enabled` transitions false→true.

11. **[severity: maint]** `core/streaming_visibility_controller.cpp:836-870`
    — ad-hoc top-K with a linear rescan for `farthest_idx` after each
    replacement. The code is correct as written (the replace only fires
    when `dist_sq < closest[farthest_idx].distance_sq`, so the inserted
    element cannot be the new farthest), but the invariant is implicit
    and the rescan is O(max_prefetch) per insertion — `O(N·K)` total.
    A future refactor to the replace condition will silently break the
    farthest tracking. — **why it matters:** fragile hand-rolled heap;
    adversarial input with max_prefetch=64 and 2000 chunks is 128k
    compares on a path that should be O(N log K). — **fix direction:**
    `std::priority_queue` / Godot's heap helpers, or at minimum a
    `DEV_ASSERT` documenting the invariant the rescan depends on.

12. **[severity: perf]** `core/streaming_vram_regulator.cpp:438-451` —
    `can_load_more_chunks` returns `current_loaded < current_max_chunks / 2`
    when in warning. Integer division — with `current_max_chunks == 1`
    (reachable: `min_chunks` is `MAX(1u, config.min_chunks)`), this is
    `current_loaded < 0`, unsigned, always false. The loader stalls
    completely until the regulator raises `current_max_chunks`, which it
    won't while usage is in the warning band. — **why it matters:** a
    low `min_chunks` config combined with warning-band usage deadlocks
    loading; user-visible stall without an error path. — **fix direction:**
    `current_loaded < MAX(1u, current_max_chunks / 2)`, and document
    `min_chunks >= 2` as the supported floor.

13. **[severity: perf]** `core/streaming_telemetry_adapter.cpp:1-29` — the
    whole file is 29 lines, and the two public functions both call the
    same `_apply_queue_pressure_common`. There is no reason for two
    functions. — **why it matters:** zero functional bug; it is a
    readability/API-surface smell. The caller has to pick
    `analytics` vs. `diagnostics` and there is no behavioural difference.
    — **fix direction:** collapse to one public function; the caller
    passes the dictionary they want populated.

14. **[severity: maint]** `core/streaming_queue_pressure_controller.cpp:119-165`
    — the `reason` classification is a **tower of if/else with exclusion
    lists**. Each branch says "this cap AND NOT all the other caps".
    Eleven such branches; the ordering matters; any new cap has to be
    added to N places. This is the exact shape of a table-driven
    classifier. — **why it matters:** adding a new cap is a merge
    conflict magnet; the exclusion lists are silently incomplete (the
    `sync_queue_cap` reason token is never produced — I checked). —
    **fix direction:** represent caps as a bitmask, derive `reason` from
    `popcount + first-set` via a lookup table; the taxonomy becomes a
    data structure instead of a control-flow maze.

15. **[severity: perf]** `core/streaming_eviction_controller.cpp:158-172`
    — the non-primary LRU candidate list is rebuilt **per-frame** with a
    full sort, regardless of whether it was used last frame. On a
    multi-asset setup with 5 atlas assets × 256 chunks = 1280 candidates
    resorted every frame. The `std::sort` lambda is 10 comparisons per
    element worst case. — **why it matters:** 10M compares/sec CPU cost
    at 60Hz on moderate scenes, spent entirely on unused sorted data
    if no eviction triggers. — **fix direction:** cache by `(residency
    generation counter, asset count)` key; only rebuild when residency
    actually changed.

---

## Cross-cutting patterns

- **Validators written, not invoked.** `streaming_queue_pressure_controller`
  has a full invariant validator suite that only tests call. This
  reproduces the main audit's finding at the pipeline-contract layer —
  the pattern repeats at the streaming layer. *Call your own functions.*
- **Per-frame cache keyed only on `total_frame_count`.** Appears in
  the eviction controller (cached visible/nonvisible split) and in the
  non-primary LRU candidate list. In both cases the cache is correct
  for the first call per frame and subtly stale on repeat calls. A
  generation counter bumped on any residency mutation would fix both
  sites with one change.
- **`uint32_t` arithmetic without saturation guards.** VRAM regulator
  (`current_max_chunks - step`), scan budget math
  (`scan_budget / (depth_excess + 1u)`), pressure headroom computation.
  On healthy inputs these are fine; under thrashing they flip sign and
  the code keeps going. The project has a `MAX<uint32_t>(1u, ...)` idiom
  in places — use it consistently.
- **Monolithic per-chunk loops.**
  `StreamingVisibilityController::update_chunk_visibility` is 175 lines
  and walks all chunks; `update_chunk_lod_blend_factors` and
  `update_chunk_lod_parameters` each walk all chunks again; the eviction
  controller walks all chunks to rebuild the per-frame cache. These could
  be a single fused pass per frame driven by a shared visitor.
- **Code duplication between grid and non-grid paths** in the visibility
  controller. Same shape, two copies, already drifting.
- **Function-local `static`** as a log throttle appears twice. No shared
  helper exists; the pattern is re-invented each time. A frame-counter-
  based throttle on `GaussianStreamingSystem` would kill both.
- **`streaming_telemetry_adapter` is over-engineered for what it does.**
  It's two trivial functions. The same pattern of "pre-bake a snapshot
  struct, hand to an adapter" would pay for itself if applied broadly;
  applied once, the adapter looks like bureaucracy.
- **Eviction ownership inverted.** `evict_non_primary_lru` records its
  own eviction; `evict_least_recently_used` does not; callers record for
  it. Two callers of `evict_least_recently_used`
  (`gaussian_streaming.cpp:2833, 4635`) remember to call
  `record_eviction_result`. Any future caller that forgets misses the
  counter. Inconsistent ownership = latent counter drift.

---

## Recommended refactor moves

### P0 — ship-blocking (1–3 days)

- **Fix VRAM regulator uint32 underflow** (issue #1). Two lines + one
  regression test. Hours.
- **NaN guard on quantization** (issue #2). One `ERR_FAIL_COND` at top
  of `ChunkQuantizationInfo::compute_from_gaussians`, one
  `Math::is_finite` clamp inside `quantize_position`. Hours.
- **Fix LOD transition double-count** (issue #4). Three lines deleted.
  Half a day including a unit test.
- **Remove `can_load_more_chunks` deadlock** (issue #12). One-line
  change + document `min_chunks >= 2` supported floor. Hours.

### P1 — quality gate (1 week)

- **Call your own validators** (issue #8). Add `DEV_ASSERT(
  validate_summary_invariants(summary, sample, nullptr))` at the bottom
  of `summarize()`. Same for latched state. Day, including expanding
  test coverage of the validator itself.
- **Replace per-frame eviction cache key** (issue #6). Introduce a
  `residency_generation_counter` on `GaussianStreamingSystem`; bump
  on every `_complete_chunk_load_common` and `_unload_chunk`; key both
  eviction caches on `(generation, frame)`. Half-day.
- **De-duplicate grid vs. non-grid visibility loops** (issue #9).
  Extract a `_classify_chunk(i, camera_pos, frustum_planes)` helper;
  call from both paths. Half-day.
- **Make `record_total_eviction` either real or delete it** (issue #5).
  Prefer: give the atlas path an `EvictionResult`. Half-day.
- **Move static log counters onto the system** (issue #3). One hour.

### P2 — structural (2–3 weeks)

- **Extract `StreamingLODPolicy` class with its own header** (issue #7).
  Move `_update_chunk_lod_parameters` / `_update_chunk_lod_blend_factors`
  / `get_lod_debug_stats` / `_collect_lod_debug_stats` onto it; give it
  a `Dependencies`-free interface (take `GaussianStreamingSystem &`).
  This also matches the pressure-controller/eviction-controller file
  shape, so the naming inconsistency disappears.
- **Table-drive the pressure reason classifier** (issue #14). Replace
  the if/else tower with a cap-bitmask → reason lookup. Encode in a
  single `constexpr std::array`. Days.
- **Collapse the three per-chunk loops into one fused pass** (issue #10).
  Single visitor per frame that computes distance, culls, flags
  visibility, updates LOD params, updates blend factor. Two-day rewrite
  with risk; write the invariant test first.
- **Replace insertion sort in visibility controller** (issue #9 continued).
  Use `SortArray<>` with distance comparator.

### P3 — nice-to-haves

- Collapse `StreamingTelemetryAdapter` to one function (issue #13).
- Replace ad-hoc top-K in prefetch candidate collection with
  `std::priority_queue` (issue #11).
- Add a `doc/` entry explaining the `streaming_lod_policy.cpp`-without-
  header decision until the P2 extraction lands.

---

## Blind spots

- **I did not read `gaussian_streaming.cpp`/`.h` in full** (~5,000 lines,
  explicitly out of slice). My caller analysis is grep-driven and
  targeted at the interface points I quote; a second-order bug in how
  the system composes these controllers (e.g. a lock held around
  eviction that would make the cache-invalidation issue #6 a non-issue)
  could change the severity picture.
- **Thread model.** I assume the eviction controller, visibility
  controller, and VRAM regulator are touched from a single thread
  (the streaming/main thread). The existence of `pack_jobs_in_flight`
  as `atomic<uint32_t>` on the upload pipeline and `async_pack_enabled`
  suggest there IS a worker thread model. If the eviction cache is
  ever touched from the worker, **every** severity in this audit
  escalates — the caches, the `chunk_load_counter`, the `last_used_frame`
  touch are all unsynchronized. The main audit flagged a global HashMap
  being mutated cross-thread in `gaussian_streaming.cpp`; this unit
  should be audited under the same lens.
- **I did not validate the spatial-grid rebuild trigger.** The grid is
  rebuilt when `spatial_grid.built_for_chunk_count != chunk_count`. This
  is a cardinality-based trigger — it does not fire on chunk centre
  movement or bounds change. I assume chunks are immovable once added;
  if that is ever violated, the grid returns wrong results silently.
  Verify in the chunk-construction path (`streaming_upload_pipeline`?).
- **I spot-checked five call sites** (`gaussian_streaming.cpp:2620,
  2833, 2893, 3447, 4646`, plus the `streaming_atlas.cpp:124` call of
  `record_total_eviction`). There may be additional call sites that
  follow a different convention for `record_eviction_result` ownership.
- **Tests were not read.** The main audit says 81 test files + 973
  asserts exist. A test-first eye on the pressure-controller validator
  coverage and the eviction-controller hysteresis path would sharpen
  the P1 recommendations.
- **`streaming_quantization.cpp` calls `_create_gpu_quantization_data`
  on a `const` method** and that path is safe, but the dirty-flag
  protocol (`quantization_cpu_cache_valid`, `quantization_dirty`) is
  mutated from what look like several sites in `gaussian_streaming.cpp`
  (lines 1073, 1143, 1145, 1562, 1563, 1565, 1619, 1620, 1660, 1661,
  2234). Whether all mutators are covered — particularly on atlas
  removal — is a Blind spot.
- **I did not verify that `chunk_load_counter` wraparound is safe.**
  `uint64_t` will take 5.8e11 years at 1B loads/sec, so wraparound is
  not a practical concern; but an assertion that
  `chunk_load_counter < UINT64_MAX` before the `++` would be cheap
  insurance.

---

## References (15 cited; 5 spot-checked into adjacent files)

- `modules/gaussian_splatting/core/streaming_eviction_controller.h:18-51`
- `modules/gaussian_splatting/core/streaming_eviction_controller.cpp:29-213`
- `modules/gaussian_splatting/core/streaming_queue_pressure_controller.h:55-93`
- `modules/gaussian_splatting/core/streaming_queue_pressure_controller.cpp:72-335`
- `modules/gaussian_splatting/core/streaming_visibility_controller.h:22-50, 81-145`
- `modules/gaussian_splatting/core/streaming_visibility_controller.cpp:41-99, 215-353, 418-510, 608-722, 760-890`
- `modules/gaussian_splatting/core/streaming_lod_policy.cpp:1-202`
- `modules/gaussian_splatting/core/streaming_vram_regulator.h:11-126`
- `modules/gaussian_splatting/core/streaming_vram_regulator.cpp:232-460`
- `modules/gaussian_splatting/core/streaming_telemetry_adapter.h:1-25`
- `modules/gaussian_splatting/core/streaming_telemetry_adapter.cpp:1-29`
- `modules/gaussian_splatting/core/streaming_quantization.h:33-95`
- `modules/gaussian_splatting/core/streaming_quantization.cpp:11-391`
- `modules/gaussian_splatting/core/streaming_config_overrides.h:10-39`
- Spot-check — caller contract verification:
  - `modules/gaussian_splatting/core/gaussian_streaming.cpp:2617-2688`
    (`_reset_per_frame_counters`, `_evict_for_vram_budget`)
  - `modules/gaussian_splatting/core/gaussian_streaming.cpp:2820-2870`
    (`_load_visible_chunks` eviction-then-load path)
  - `modules/gaussian_splatting/core/gaussian_streaming.cpp:2872-2896`
    (`_build_visible_chunk_list` — site of `touch_chunk_use`)
  - `modules/gaussian_splatting/core/gaussian_streaming.cpp:3443-3513`
    (`_complete_chunk_load_common`, `_unload_chunk`, eviction forwarders)
  - `modules/gaussian_splatting/core/streaming_atlas.cpp:124, 153`
    (the `record_total_eviction` and `touch_chunk_use` atlas call sites)

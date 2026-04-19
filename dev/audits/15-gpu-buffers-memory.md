# GPU Buffers & Memory — Deep Audit

Unit 15. ~3,500 LOC across the GPU buffer, memory-streaming, perf-monitoring, async-readback, and owner-mismatch contract surfaces of `modules/gaussian_splatting/renderer/`.

## Summary

**Grade: C**

The slice is a *collection* of two maturity tiers glued together:

- A clean RAII wrapper (`GPUBuffer` in `gpu_buffer_raii.h`) — textbook correct, and essentially **unused**. One benchmark site in `render_sorting_orchestrator.cpp:460-461` uses it; the rest of the slice manages raw `RID` + raw `RenderingDevice*` pairs manually, all 3,000+ lines.
- A hand-rolled ownership/state machine in `GPUBufferManager` and `GaussianMemoryStream` that *mostly* got the #257 RID-leak family right by going through `RenderDeviceManager::free_owned_resource()` for `GaussianMemoryStream`, but is **still leaking the pattern** for `GPUBufferManager` (no `device_manager` reference, no `track_resource`, no cross-device instance-id verification at free time — just raw `device->free(rid)` with a best-effort submission lock).

The delayed-free queue (`DeferredDeletionQueue` in `gpu_buffer_manager.h:29-95`) is well-written — frame-delay ring, auto-free type detection — but **is never actually used anywhere** in the audited slice. It's dead code sitting next to code that would benefit from it.

The preventive structure against the *next* #257 is partial: `ResourceOwnerMismatchContract::is_device_generation_valid()` exists and is conceptually right, but the two biggest allocators in this slice (`GPUBufferManager` storage buffers, `GaussianMemoryStream` fallback direct-allocation path) never consult it at free time. The `StreamBuffer` struct is the only one that stores an `ObjectID`/`gpu_allocation_device_id` pair, and even there the check is only in the free-path through `device_manager`, which may be null.

Bottom line: not in crash territory today, but the attack surface for another "pool exhaustion in 2-5 minutes" regression is still wide open, and the code duplication between `GPUBufferManager` and `GaussianMemoryStream` (triple buffering, memory pool, fence tracking) means fixes have to be applied twice and drift will happen.

## What this code does

1. **`gpu_buffer_raii.h`** — 58-line move-only RAII wrapper over `(RenderingDevice*, RID)`. Free in dtor and `reset()`, transfer on move, `release()` to pull the RID out without freeing.
2. **`gpu_buffer_manager.{h,cpp}`** — `RefCounted` class holding a double-buffered (`BUFFER_COUNT = 2`) set of `(gaussian_buffer, sort_key_buffer, sorted_indices_buffer, fence)` RIDs plus a uniform buffer. Provides `begin_frame / swap_buffers / end_frame`, fence-delay-based "is ready for update" checks, CPU upload of packed gaussians, and read/write handle accessors. Contains a `DeferredDeletionQueue` nested class that is never referenced.
3. **`gpu_memory_stream.{h,cpp}`** — `GaussianMemoryStream` + `StreamingPipeline`. Triple-buffered (`BUFFER_COUNT = 3`) version of GPUBufferManager with `std::atomic<BufferState>` state machine, a free-list `MemoryPool` allocator (first-fit, offsets only — Godot doesn't expose sub-allocation so the "pool" just tracks bookkeeping), per-buffer fence tokens against a shared `upload_timeline` atomic counter, and an optional worker `StreamingPipeline` thread doing LOD-ranged uploads.
4. **`gpu_performance_monitor.{h,cpp}`** — `FrameCompletionTracker` polls `RenderingDevice::get_captured_timestamps_frame()` and converts the wrapping 3-slot index into a monotonic frame counter. `GPUPerformanceMonitor` maps frame_id → `{submit_frame_index, complete_frame_index, stall_count}`, prunes at soft/hard limits, and time-thresholds stalls at 16 ms.
5. **`gaussian_gpu_layout.{h,cpp}`** — 743-line header of std140/std430-matched POD structs (`PackedGaussian`, `TileRenderParamsGPU`, `InstanceDataGPU`, etc.) with exhaustive `static_assert(offsetof(...))` guards. Also the CPU packers (`pack_gaussian`, `pack_gaussians_range`, chunked FP16 variants, RGB9E5 encoder).
6. **`batched_async_readback.{h,cpp}`** — consolidates up to 8 GPU→CPU readback requests into one staging buffer + one `buffer_get_data_async()` call, dispatches per-request callbacks with sliced data.
7. **`resource_owner_mismatch_contract.{h,cpp}`** — small free-function contract: `evaluate(Inputs) → Decision` for deciding whether a resource should be released or force-invalidated, plus static helpers for texture/buffer ownership verification and device-generation validation.
8. **`gpu_debug_utils.h`** — `ScopedGpuMarker` RAII marker helpers with pass-color macros, and a `GS_SAFE_FREE(device, rid, tag)` macro that wraps `device->free(rid); rid = RID();` with optional logging.

## RID lifecycle analysis

### Allocation paths (4 distinct, inconsistently tracked)

1. **`GPUBufferManager::_create_buffer_set`** (`gpu_buffer_manager.cpp:119-159`) → `device->storage_buffer_create(...)`. Stores `RenderingDevice* device` on the `BufferSet`. **No** `track_resource()` call. **No** `ObjectID` / `get_device_instance_id()` capture.
2. **`GPUBufferManager::create_buffers` → uniform_buffer** (`gpu_buffer_manager.cpp:212-222`) → `device->uniform_buffer_create(...)`. Stores raw `RenderingDevice *uniform_buffer_device`. **No** tracking.
3. **`GaussianMemoryStream::_create_buffer`** (`gpu_memory_stream.cpp:183-242`) → `storage_buffer_create`. Captures both raw pointer **and** `gpu_allocation_device_id = submission_device->get_instance_id()`. If `device_manager` is set, calls `track_resource(... owned=true ...)`. **This is the only allocation path in the slice that is generation-tagged.**
4. **`BatchedAsyncReadback::initialize`** (`batched_async_readback.cpp:40-49`) → `rd->storage_buffer_create`. Stores raw `rd` pointer. **No** tracking.

### Free paths (5 distinct)

1. **`GPUBufferManager::_destroy_buffer_set`** (`gpu_buffer_manager.cpp:161-184`) — straight `device->free(rid)` without validity check, via `_acquire_submission_device(p_set.device ? p_set.device : rd, …)`. **If `p_set.device` was destroyed since allocation, this is a use-after-free** (raw pointer comparison in `acquire_submission_device` can't tell). The `ResourceOwnerMismatchContract::is_device_generation_valid()` helper that would catch this is never called.
2. **`GPUBufferManager::cleanup_buffers` → uniform_buffer** (`gpu_buffer_manager.cpp:390-398`) — same issue; fallback is `rd` if `uniform_buffer_device` is null.
3. **`GPUBufferManager::_mark_buffer_ready` → fence** (`gpu_buffer_manager.cpp:232-250`) — the `set.fence` RID is always `RID()` in this code (it's never assigned anywhere I can find — dead data path), so the `device->free(set.fence)` branch is dead.
4. **`GaussianMemoryStream::_destroy_buffer`** (`gpu_memory_stream.cpp:244-282`) — goes through `_resolve_device(...)` which *does* validate the `ObjectID`, falls back to `rd` only if owner resolves. Then prefers `device_manager->free_owned_resource(...)` which applies the full #257 fix (auto-free type detection, instance-id check, forget-on-mismatch). This is the **best** free path in the slice.
5. **`BatchedAsyncReadback::shutdown`** (`batched_async_readback.cpp:52-69`) — raw `rd->free(staging_buffer)` with no validity check and no generation tag. If the RD died before shutdown ran — and `shutdown()` is the dtor path — use-after-free.

### #257 prevention — coverage matrix

Only `GaussianMemoryStream` routes through `RenderDeviceManager::free_owned_resource`, which after the #257/#258 fix explicitly calls `device->free()` on valid AUTO_FREE_TYPES before forgetting them. `GPUBufferManager` and `BatchedAsyncReadback` have no `Ref<RenderDeviceManager>` member at all — every free is a raw `device->free(rid)`. The `DeferredDeletionQueue` nested class (`gpu_buffer_manager.h:29-95`) has the right pattern (auto-free validity check + explicit free) but is **instantiated nowhere** — dead code. Generation-tagging plumbing exists (`ResourceOwnerMismatchContract::is_device_generation_valid` + `RenderingDevice::get_device_instance_id`) but only `StreamBuffer` actually stores the id.

### Where UAF could land

- **If `GPUBufferManager` outlives the rendering device** (e.g. during shutdown ordering edge cases), `cleanup_buffers()` dereferences `uniform_buffer_device` / per-set `device` via `acquire_submission_device`, which also dereferences the pointer. No generation check. Godot's rendering server teardown order normally saves this, but the comment in `painterly_material_manager.cpp` ("Codex flagged that the cached `rd` pointer could be stale at destructor-time teardown") in commit 79ecf98 confirms this exact pattern is a real issue in sibling code, and `GPUBufferManager` has no equivalent `_is_rd_alive()` guard.
- **If `GaussianMemoryStream::shutdown()` is called without `device_manager` set** (it is an optional field, settable via `set_device_manager()` which may never be called — the `_create_buffer` path allocates even when `device_manager.is_valid()` is false), then the `_destroy_buffer` path at `gpu_memory_stream.cpp:264-268` falls into `allocation_device->free(buffer.gpu_buffer)` with only `_resolve_device` as protection. Still better than `GPUBufferManager`'s path, but not equivalent to the manager path's #257 fix.
- **Batched readback staging buffer** uses `rd->free(staging_buffer)` in `shutdown()` (called from dtor). If `rd` was destroyed first — use-after-free. There is no `buffer_is_valid()` guard, no generation tag.

## Strengths

1. **`static_assert` carpet** in `gaussian_gpu_layout.h` — every struct has offset/size/alignment assertions keyed to the std140/std430 GPU layout (lines 88-96, 104-106, 125-132, 152-159, 165-167, 175-187, 307-318, 462-509, 534-554, 568-577, 594-610). If these ever drift from shader layouts, compilation fails immediately. This is the most valuable code in the slice.
2. **`DeferredDeletionQueue` design** (`gpu_buffer_manager.h:29-95`) is correct: frame-delay ring, in-place compaction on `process_frame`, auto-free type detection. Unused, but well-written.
3. **`FrameCompletionTracker`'s monotonic conversion** (`gpu_performance_monitor.cpp:14-45`) correctly handles the 3-slot wrap in `get_captured_timestamps_frame()` to avoid false stall triggers.
4. **`BatchedAsyncReadback` partial-failure handling** (`batched_async_readback.cpp:120-147`) — if some `buffer_copy`s fail, continue with the rest rather than canceling, then skip those callbacks at dispatch time via `request.callback = Callable()` invalidation. The explicit bounds check at dispatch (`batched_async_readback.cpp:184-190`) guards against short readbacks.
5. **`ResourceOwnerMismatchContract::evaluate()`** (`resource_owner_mismatch_contract.cpp:15-33`) is a pure function with a stated invariant, easily unit-testable. The `validate()` companion enforces that invariant symbolically. Correct design pattern for a safety contract.
6. **Move-only semantics on `GPUBuffer`** (`gpu_buffer_raii.h:36-37`) and on both `ScopedGpuMarker` variants (`gpu_debug_utils.h:117-120, 145-148`). Copy/move are properly deleted. These classes can't accidentally double-free.
7. **`GaussianMemoryStream` pool-vs-direct accounting** (`gpu_memory_stream.cpp:193-213, 244-269`) correctly distinguishes `from_pool` and returns the offset slot on destroy, even though the "pool" is just bookkeeping (no real sub-allocation — comments at line 937-944 are honest about this).
8. **`defragment_if_needed` is disabled with a loud warning** (`gpu_memory_stream.cpp:819-834, 1005-1010`) rather than silently corrupting allocations. Good judgment call — unsafe defrag is known to the authors.

## Top issues

1. **[crash] `gpu_buffer_manager.cpp:161-184` + `batched_async_readback.cpp:52-69` — raw `device->free(rid)` through a cached `RenderingDevice*` with no generation check and no `buffer_is_valid()` gate** — If the device this RID was allocated on was destroyed/recycled (engine/editor restart, world transition, subviewport teardown), `acquire_submission_device(p_set.device, …)` dereferences a dangling pointer before the `free()`. `painterly_material_manager.cpp` gained an `_is_rd_alive()` guard after Codex caught this exact class in PR #258; `GPUBufferManager` and `BatchedAsyncReadback` have nothing equivalent. `BatchedAsyncReadback::shutdown()` is invoked from the dtor, so a post-world-switch teardown is the most likely trigger. **Fix: store `get_device_instance_id()` alongside every cached pointer, call `ResourceOwnerMismatchContract::is_device_generation_valid()` before dereferencing, or route through `RenderDeviceManager::free_owned_resource` as `GaussianMemoryStream` does.**

2. **[crash] `gpu_memory_stream.h:191` + `gpu_memory_stream.cpp` — declared-but-never-defined methods**: `get_sort_keys_buffer()`, `update_visible_range(start, count)`, `set_lod_ranges(...)`, `update_culling_mask(...)`. `tests/performance_benchmark.cpp:375, 398` actually call `memory_stream->get_sort_keys_buffer()`. With `tests=yes`, this is a **linker error**; at runtime via ClassDB, undefined. Headers silently lie about the API surface. **Fix: implement them or delete the declarations and the test call sites.**

3. **[corruption] `gpu_buffer_manager.h:29-95` — `DeferredDeletionQueue` is dead code** — The nested class has the correct pattern for delayed frees (frame-delay ring + auto-free validity check) which is precisely what would have prevented #257. But `GPUBufferManager` never instantiates it, never calls `process_frame`, never calls `queue_free`. Meanwhile `_destroy_buffer_set` frees synchronously with no delay, ignoring the `in_flight_delay` fence tracking already maintained in the same class. **Fix: wire it up (one member, two call sites) or delete it.**

4. **[corruption] `gpu_buffer_manager.cpp:541-615` — `upload_gaussian_data` auto-swap-and-end-frame when `!frame_was_active`** — opens a frame internally, does the upload with `safe_submit_and_sync` at line 594 (blocking), then unconditionally calls `swap_buffers()` and `end_frame()`. If any other code path already called `begin_frame()` externally and expected to drive the swap, this tramples frame state and advances `frame_index` out from under the caller. `frame_was_active` is checked at entry, but nothing prevents re-entry between that check and `_begin_frame_internal` on line 545. **Fix: ERR_FAIL when mid-frame entry is detected, or document non-re-entrancy and assert.**

5. **[perf] `gpu_buffer_manager.cpp:594` — synchronous `safe_submit_and_sync` on every upload** — the manager advertises double-buffering to decouple writers from readers, but every upload does a full CPU stall. Combined with the auto-end-frame path (#4), a single upload is a full GPU round-trip and the entire `in_flight`/`fence_delay` machinery is defeated by this one line. **Fix: downgrade to `safe_submit` (no sync) and trust the next-frame wait.**

6. **[crash] `gpu_memory_stream.cpp:786-797` — `swap_buffers` CAS from `BUFFER_RENDERING` to `BUFFER_FREE` without waiting for GPU finish** — If the render pass against the previous buffer is still executing when `swap_buffers` runs, state flips to FREE and `_get_next_write_buffer` can immediately re-acquire it for an upload, racing the live draw. The class has fence tracking for the upload direction but nothing equivalent for the render direction. The `frame_last_used = current_frame` stamp at line 791 is only consulted during READY reuse (line 316-335), not during the FREE transition. **Fix: gate the FREE transition on frame-delay age, or move freed-but-not-yet-safe state into a post-swap queue.**

7. **[corruption] `gpu_memory_stream.cpp:316-335` — READY-buffer reuse fallback in `_get_next_write_buffer` does not advance `write_index`** — the two primary loops at 286-297 and 306-314 correctly advance `write_index = (idx + 1) % BUFFER_COUNT` after a successful CAS. The READY-reuse fallback (316-335) does not — the next call retries the same index, which is now UPLOADING, spins one lap, and falls through to stall. **Fix: `write_index = (ready_idx + 1) % BUFFER_COUNT` after the READY-to-UPLOADING CAS succeeds.**

8. **[maint] `gpu_buffer_manager.h:104` + `gpu_buffer_manager.cpp:232-250` — `BufferSet::fence` RID exists but is never assigned** — `_mark_buffer_ready` and `_destroy_buffer_set` both `free()` it, but no allocation site ever populates it. Godot 4 doesn't expose timeline semaphores (as `gpu_performance_monitor.h:10-11` states explicitly). Dead field. **Fix: delete it, or implement true fence-based completion.**

9. **[perf] `gpu_memory_stream.cpp:799-817` — `compact_memory` is O(N²)** — `do { for (i < blocks.size()-1) { if (merge) { merged=true; break; } } } while (merged);` restarts the entire scan on every merge. Pathological on fragmented pools. **Fix: single linear walk, merging right-neighbors in place.**

10. **[maint] `gpu_buffer_manager.cpp:707-722` — `set_visible_count` uses unclamped `p_visible` when `gaussian_count == 0`** — line 714: `uint32_t clamped = maximum > 0 ? MIN(p_visible, maximum) : p_visible;`. If the read buffer is uninitialized, `set_visible_count(10_000_000)` is accepted verbatim and propagated to downstream culling/sort stages as dispatch size. Overruns the buffer. **Fix: always clamp to `max_gaussians`.**

11. **[perf] `batched_async_readback.h:17` — `MAX_READBACK_SLOTS = 8` fixed cap, no chain-through** — `add_request` returns `false` when full, forcing callers to drop readbacks or spin on `submit_batch`. **Fix: raise the cap, or auto-submit and chain a new batch.**

12. **[maint] `gpu_buffer_manager.cpp:650-654, 571-575` + `gaussian_gpu_layout.cpp:93-96, 134-152, 171-179` — `static int dbg_count` counters in hot accessors** — race under concurrent callers, log once per process rather than per instance, and leak debug instrumentation into production getters. **Fix: route through `GS_LOG_GPU_MEMORY_DEBUG` with a rate-limit helper.**

## Cross-cutting patterns

- **Raw `RenderingDevice*` + bare `RID` is the dominant ownership model**, despite `GPUBuffer` RAII sitting right there unused. Every file in the slice has 5-30 raw `device->free(rid)` calls — the exact pattern #257 was about.
- **Two parallel implementations of "multi-buffered streaming with fence delay"**: `GPUBufferManager` (double-buffered) and `GaussianMemoryStream` (triple-buffered, atomic state machine, pool). They share no code. They will drift.
- **Generation-tagging is half-built**: `RenderingDevice::get_device_instance_id()`, `ResourceOwnerMismatchContract::is_device_generation_valid()`, and `RenderDeviceManager`'s instance-id check all exist. Only `StreamBuffer` captures the id, and only one free path uses it.
- **Three flavors of submission lock acquisition** with subtly different null-singleton fallbacks: direct `gs_device_utils::safe_submit_and_sync(device)`, `GaussianSplatManager::get_singleton()->acquire_submission_device(...)`, and the per-file `_acquire_submission_device` namespaced helper.
- **`static_assert` discipline in `gaussian_gpu_layout.h` is strong but doesn't extend to runtime checks** — `pack_gaussians_range_raw`'s `LocalVector` overload (line 197-219) bounds-checks; its raw-pointer twin (line 221-241) can't and doesn't.

## Recommended refactor moves

### P0 — risk of next #257-class bug (effort: ~2 days)

- Adopt `GPUBuffer` RAII (or a slightly extended `TrackedGpuBuffer` that carries `device_instance_id`) **inside `GPUBufferManager::BufferSet`** and inside `BatchedAsyncReadback::staging_buffer`. At a minimum, capture `device_instance_id` alongside the raw pointer; verify it with `ResourceOwnerMismatchContract::is_device_generation_valid` before every `device->free`.
- Make `GPUBufferManager` take `Ref<RenderDeviceManager>` and route all frees through `free_owned_resource`, same as `GaussianMemoryStream`. Delete the raw-`device->free` calls at `gpu_buffer_manager.cpp:171-181, 243, 394`.
- Delete the `BufferSet::fence` RID field or actually implement fence-based completion — dead fields hide behind allocation sites.

### P1 — structural (effort: ~1 week)

- Either wire up `DeferredDeletionQueue` to every allocator in the slice (giving a real frame-delay-before-free for uniform sets and pipelines, matching the #257 fix contract) or delete it from the header. Dead code pretending to be a safety net is worse than no safety net.
- Unify `GPUBufferManager` and `GaussianMemoryStream`: one `MultiBufferedStorage<N>` template parameterized on buffer count, plus one `GaussianUploadPolicy` that knows how to pack and stream. The current duplication is unmaintainable — any real-world fix has to be written twice.
- Implement or delete the ghost methods on `GaussianMemoryStream`: `get_sort_keys_buffer`, `update_visible_range`, `set_lod_ranges`, `update_culling_mask`. Tests call into one of them today, so this is a live link-time bomb.
- Fix `_get_next_write_buffer` fallback-path write_index update (#7 above) — 1-line bug but it causes quiet perf degradation under backpressure.

### P2 — correctness polish (effort: ~2 days)

- Replace `static int dbg_count` debug counters with rate-limited log helpers throughout the slice.
- Linearize `MemoryPool::compact_memory` — current `do/while(merged)` is quadratic.
- Clamp `set_visible_count` to `max_gaussians` unconditionally, not only when `gaussian_count > 0`.
- Document (and assert) that `upload_gaussian_data` is not re-entrant with an externally-owned frame pair.

## Blind spots

- **Out of slice**: the actual consumers of these buffers (`render_sorting_orchestrator.cpp`, `gaussian_splat_renderer.cpp`, `gpu_sorter.cpp`, `painterly_pass_graph.cpp`) may or may not hold their own RIDs to the same resources. I did not audit their free paths, but their ownership model is relevant to the question of whether the in-slice RIDs ever get freed externally.
- **`RenderDeviceManager` implementation details** — I sampled the `free_owned_resource` body (lines 385-530) and read the API, but did not audit `track_resource` or the `texture_owner_map`/`resource_owner_map` consistency. A bug in the manager would propagate to the one allocator that trusts it (`GaussianMemoryStream`).
- **Godot's internal storage buffer usage flags** — `storage_buffer_create` has a `p_usage` bitfield that includes `STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT`, etc. None of the call sites in this slice pass any usage flags. Whether Godot's default gives us TRANSFER_SRC/DST for `buffer_copy` in `BatchedAsyncReadback::submit_batch` without explicit opt-in is a driver detail I did not chase.
- **`buffer_get_data_async` callback threading model** — `_on_batch_readback` is invoked via `callable_mp(this, &…)` from Godot's RD. Whether the callback runs on the RD thread, the main thread, or a pool thread determines whether the `batch_state` / `submitted_requests` access is racy. I assumed main-thread delivery; if it's RD-thread, `add_request` + `_on_batch_readback` need a mutex that isn't there.
- **`DeferredDeletionQueue::process_frame` atomic semantics** — if it ever gets wired up, callers need to guarantee `process_frame` is called on exactly one thread. The class has no internal mutex.
- **Didn't spot-check**: `float16_utils.cpp`, `gpu_sorting_config.cpp`, full `gaussian_splat_manager.h` submission-lock semantics, the concrete SPIRV-side `std140` / `std430` shader layouts that `gaussian_gpu_layout.h` asserts against (the `static_asserts` guard the C++ side — if shader side drifts nothing catches it).

---

**Files cited**: `modules/gaussian_splatting/renderer/gpu_buffer_raii.h`, `gpu_buffer_manager.h`, `gpu_buffer_manager.cpp`, `gpu_memory_stream.h`, `gpu_memory_stream.cpp`, `gpu_performance_monitor.h`, `gpu_performance_monitor.cpp`, `gaussian_gpu_layout.h`, `gaussian_gpu_layout.cpp`, `batched_async_readback.h`, `batched_async_readback.cpp`, `resource_owner_mismatch_contract.h`, `resource_owner_mismatch_contract.cpp`, `gpu_debug_utils.h`; cross-refs: `interfaces/render_device_manager.{h,cpp}`, `interfaces/gpu_sorting_pipeline.cpp`, `renderer/render_sorting_orchestrator.cpp`, `tests/performance_benchmark.cpp`, `servers/rendering/rendering_device.h`, commit `79ecf98c1f` (#257 fix).

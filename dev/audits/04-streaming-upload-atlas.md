# Streaming Upload & Atlas — Deep Audit

Unit 04 — Streaming Upload & Atlas slice (~2.7k LOC):
- `modules/gaussian_splatting/core/streaming_upload_pipeline.{h,cpp}` (~1.7k LOC)
- `modules/gaussian_splatting/core/streaming_atlas.{h,cpp}` (202 LOC)
- `modules/gaussian_splatting/core/streaming_global_atlas_registry.{h,cpp}` (~640 LOC)
- `modules/gaussian_splatting/core/streaming_chunk_payload_source.{h,cpp}` (~230 LOC)
- `modules/gaussian_splatting/core/streaming_runtime_state.h`, `streaming_asset_types.h`

## Summary

**Grade: C+**

Functionally correct on the fast path; defensively guarded to an almost paranoid degree; heavily instrumented. Enough bug-fix iterations have landed that the obvious slot-ownership and generation-mismatch races are now gated by explicit invariants.

Cost of that hardening: overlapping validation where each code path re-checks slot-tracking, chunk state, and allocator consistency three-or-four times and repairs divergences inline. The upload pipeline is a ~1700-line file with one 477-line god method (`process_upload_queue`), five nested lambdas, a hand-rolled coalescer, manual queue compaction, and public mutex/queue fields that break encapsulation. The slot allocator itself is clean but purely LIFO — which sabotages the coalescing logic sitting on top of it. The global atlas registry uses `WARN_PRINT_ONCE` + "force rebuild" recovery in place of data-structure invariants.

No confirmed crash; no confirmed memory corruption. Latent perf hazards: per-read `FileAccess::open`, double-hashing of packed payloads, O(total_chunks) cleanup scan on every device-loss recovery. Biggest maintainability debt: `StreamingUploadPipeline` is effectively a public-member-friend of `GaussianStreamingSystem`.

## What this code does

The streaming subsystem owns residency of Gaussian chunks in a single persistent GPU "atlas" buffer. Each chunk is a fixed-size slot of `CHUNK_SIZE * sizeof(PackedGaussian)` bytes.

1. **Atlas slot allocator** (`streaming_atlas.h/.cpp`): a LIFO free-list indexed by a 64-bit `chunk_key = (asset_id << 32) | chunk_idx`. Allocates/releases slot indices [0..capacity). Pure main-thread.
2. **Global atlas registry** (`streaming_global_atlas_registry.{h,cpp}`): maintains CPU-side mirrors of `AssetMetaGPU`, `ChunkMetaGPU`, `AssetChunkIndexGPU` arrays, plus dirty-index tracking for incremental uploads of per-chunk metadata. Syncs to GPU once per frame. Also owns `global_atlas_state.atlas_generation` which external consumers use for change detection.
3. **Payload sources** (`streaming_chunk_payload_source.{h,cpp}`): abstract `ChunkPayloadSource` with two impls — `InMemoryChunkPayloadSource` (wraps `GaussianData`) and `StagedFileChunkPayloadSource` (reads directly from `.gsplatworld` per call, opening a new `FileAccess` each read).
4. **Upload pipeline** (`streaming_upload_pipeline.{h,cpp}`): main thread enqueues `PackJob` records, N pack worker threads dequeue under `pack_mutex`, snapshot via payload source, pack into `PackedGaussian`, checksum, then enqueue a `PendingChunkUpload*`. Main thread drains `upload_queue` into `RenderingDevice::buffer_update` each frame with per-frame/per-slice/per-second byte budgets and a contiguous-slot coalescer.
5. **Runtime state/asset types** (`streaming_runtime_state.h`, `streaming_asset_types.h`): plain data structs (`FrameData`, `BudgetState`, `SchedulerState`, `DiagnosticsState`, `AtlasAssetState`, `StreamingChunk`, etc.).

## Strengths

- **Slot allocator is minimal and clean.** `GaussianAtlasAllocator` is ~50 lines, single-purpose, easy to reason about (`streaming_atlas.cpp:14-62`). No tricky bit-manipulation or reference counting inside. Free list + HashMap<key,slot>. Good fit for a flyweight pool.
- **Pack threads are cleanly isolated from GPU concerns.** `build_pending_upload_from_pack_job` (`streaming_upload_pipeline.cpp:1102-1171`) only reads via `Ref<ChunkPayloadSource>` and writes to a PendingChunkUpload — no atlas allocator, no GPU RID, no chunk-state mutation. That's the right separation.
- **Generation checks before GPU writes.** `resolve_upload_chunk` (`streaming_upload_pipeline.cpp:619-679`) validates `asset->generation == job->asset_generation` and rolls back stale uploads before they corrupt atlas slots — prevents the common "asset reloaded while upload in flight" class of corruption.
- **Payload checksum guards the pack→upload handoff.** `_packed_gaussian_payload_checksum` (`streaming_upload_pipeline.cpp:102-107`, verified at `:886-901`) catches packed-data corruption between pack thread and upload thread. Rare, but the error path is non-fatal.
- **StagedFileChunkPayloadSource concurrent reads work.** The explicit per-call `FileAccess::open` pattern (`streaming_chunk_payload_source.cpp:101-104, 170-174`) genuinely does let multiple pack workers read the same world file concurrently without a shared-handle mutex — valid on Win32 and Posix.
- **Atomic token-bucket for bandwidth.** `prepare_upload_budget_state` (`streaming_upload_pipeline.cpp:1383-1414`) implements a clean refill token bucket against wallclock, capped at 2× per-second budget.
- **Chunk-meta incremental upload planner.** `_plan_chunk_meta_uploads` (`streaming_global_atlas_registry.cpp:97-135`) sorts + dedupes dirty indices, then decides full-update vs contiguous-range uploads using both range-count and density thresholds. Nicely factored.

## Top issues

**[perf, maint] streaming_upload_pipeline.cpp:615** — `process_upload_queue` calls `clear_pending_uploads(system)` when the submission RD or persistent buffer is invalid, *while pack threads are still running*. A worker between `pack_queue` dequeue (line 462) and `enqueue_upload_job` (line 480) will insert a freshly-packed `PendingChunkUpload*` into the upload queue that was just emptied. The next `process_upload_queue` correctly rejects it via `resolve_upload_chunk` generation/state checks, so no corruption — but the pack work was spent, the rollback+reject churn is uninstrumented, and under repeated device-loss the pattern compounds. — *Fix direction*: add a `uint64_t upload_generation` bumped on each clear; pack_thread stamps jobs at enqueue; drop stale-generation jobs at `pop_upload_job`.

**[maint, latent-corruption] streaming_upload_pipeline.cpp:145-154** — `_release_chunk_slot_if_matches` treats `p_expected_slot == UINT32_MAX` as a wildcard: the guard at line 150 is `p_expected_slot != UINT32_MAX && mapped_slot != p_expected_slot`, so passing `UINT32_MAX` unconditionally releases whatever slot the allocator happens to map at `chunk_key`. Current callers all pass `job->buffer_slot`/`job.buffer_slot`, and those fields are stamped to a real slot at `queue_chunk_load:586` before the job enters any queue — so in today's code the wildcard is unreachable. But the helper signature advertises no such precondition, and any future caller that forwards a raw `StreamingChunk::buffer_slot` (which *is* `UINT32_MAX` after rollback) will silently steal a cross-chunk slot. — *Fix direction*: split into `_release_chunk_slot_if_matches` (strict expected, asserts `expected != UINT32_MAX`) and a separate explicit-wildcard variant; audit every new call site.

**[perf] streaming_chunk_payload_source.cpp:101, :170** — `StagedFileChunkPayloadSource::capture_chunk_snapshot` opens a brand-new `FileAccess` on every call. Under a streaming corridor at 60 Hz × 8 pack workers × sustained VRAM churn, that's tens of `CreateFile/CloseFile` pairs per frame on Windows. The comment claims this was chosen to avoid mutex contention on a single handle, which is a fair trade for correctness but a terrible one for throughput. — *Why it matters*: measurable syscall tax; Windows `CreateFile` is slow (µs range). — *Fix direction*: thread-local `FileAccess` cache keyed by `file_path`, or a small per-worker LRU of open handles.

**[perf] streaming_chunk_payload_source.cpp:154-166** — `capture_indexed_chunk_snapshot` computes `min_idx`/`max_idx` of the index list and reads the entire contiguous range `[min_idx, max_idx]` from disk, then scatter-extracts the requested splats. For *scattered* source indices (e.g., a primary-chunk layout with a shuffled source_index remap), this can read 100× the actual payload. There's no fallback to multiple smaller seeks, no threshold. — *Why it matters*: in the worst case (indices spanning a whole asset), a 1MB indexed request becomes a 100MB disk read. — *Fix direction*: if `range_count > K × p_count`, fall back to per-index seeks; or sort indices, coalesce runs, issue multi-range read.

**[perf] streaming_upload_pipeline.cpp:886 (pair with :1169)** — Packed-gaussian checksum is computed once on pack thread (`:1169`) and re-hashed on main thread at every dispatch (`:886`). Requeued chunks get re-hashed again next frame. The verify exists to catch memory corruption between pack→upload, but there's no threading mutation of `packed_data` on that path — it's a CPU→CPU integrity check against a threat model that doesn't apply. At ~2 MB/chunk with 4 GB/s hash, that's ~500 µs/chunk on the main thread — under a burst of 16 chunks, ~8 ms of pure hashing. — *Fix direction*: make the main-thread verify `DEV_ENABLED`-only.

**[latent-corruption] streaming_upload_pipeline.cpp:966** — `upload_coalescing_scratch.resize(batched_gaussian_count)` sizes the scratch for full slots (`batched_job_count * slot_capacity_bytes / sizeof(PackedGaussian)`). The memcpy loop at `:970-977` advances `scratch_offset` by the actual `source.size()`. This is consistent only because the planner at `_plan_coalesced_upload_batch:1293-1320` rejects any candidate with `packed_count != CHUNK_SIZE`. The coupling is implicit across two files. If anyone ever relaxes the planner's full-slot check, the scratch and `buffer_update` at `:979-982` will diverge and corrupt the persistent buffer. — *Fix direction*: derive scratch size from `sum(source.size())` and assert it equals `batched_job_count * CHUNK_SIZE`; or fold planner and dispatcher into one owner.

**[maint] streaming_upload_pipeline.cpp:606-1083** — `process_upload_queue` is a 477-line monolith with four stateful lambdas (`resolve_upload_chunk`, `inspect_upload_chunk_for_coalescing`, `upload_job_slices`, `finalize_upload_job`) plus inline top-level logic. Coalescer, per-job slice loop, budget accounting, and pressure-controller update are all entwined. Lambdas close over 8+ locals. Each lambda re-runs slot-match validation for defense-in-depth. Seven exit paths through `memdelete(job)`, two through `requeue_upload_job`. — *Fix direction*: extract `UploadDispatcher` class with named per-phase methods; run coalescing as a separate pre-pass before the dispatch loop.

**[maint] streaming_upload_pipeline.h:213-272** — Public fields: `pack_mutex`, `pack_semaphore`, `pack_threads`, `pack_queue`, `pack_queue_read_idx`, `upload_queue`, `upload_queue_read_idx`, `pack_jobs_in_flight`, `sync_pack_scratch`, telemetry counters. Any caller can reach in and break every invariant the cancel/clear paths carefully enforce. Friend-by-public-field. Refactoring anything in this file is blocked until these are privatized. — *Fix direction*: move all queue/mutex/thread state under `private:`; expose verbs (`queue`, `cancel`, `drain`, `has_pending`, `stop`).

**[perf] streaming_atlas.h:34 + streaming_upload_pipeline.cpp:1285-1322** — `GaussianAtlasAllocator` uses a LIFO `LocalVector<uint32_t> free_slots`: freshly-freed slots are reallocated first. Under sustained eviction churn this scatters active slots across the whole range. The coalescing planner requires `buffer_slot == expected + 1` for every additional candidate — so once VRAM is under churn, `coalesced_job_count` collapses to 1. Coalescing only works on cold-start contiguous allocations. — *Fix direction*: switch free list to a min-heap so allocations always pick the lowest free slot, restoring contiguity.

**[maint, perf] streaming_global_atlas_registry.cpp:290-309** — During `build_cpu_state`, `update_chunk_meta_entry` at `:380-388` maintains `atlas_published_chunk_count` via delta against whatever's already in `chunk_meta_cpu[global_idx]`. But `chunk_meta_cpu.resize(total_chunks)` at `:210` preserves stale entries from the previous build, so intermediate deltas diff against stale values. Then `:303-309` authoritatively overwrites the counter with a fresh scan. Two redundant accounting paths; the delta work during rebuild is pure waste. Doesn't corrupt the final value, but an observer reading `atlas_published_chunk_count` mid-rebuild would see garbage (no current observer does). — *Fix direction*: in `update_chunk_meta_entry`, skip the delta update when called from `build_cpu_state` (flag arg), or zero-init `chunk_meta_cpu` before the rebuild loop.

**[maint] streaming_global_atlas_registry.cpp:398-449** — `mark_chunk_meta_dirty` has five `WARN_PRINT_ONCE + _invalidate_chunk_meta_tracking()` bailouts for what are structural data-integrity violations (chunk_idx out of range, `chunk_meta_base` inconsistent, dirty-flags size mismatch, `global_idx` out of range). "Force a full atlas rebuild" as silent self-heal papers over the actual bug: WARN_PRINT_ONCE fires once per process while the rebuild keeps running every frame. — *Fix direction*: add a rebuild-count telemetry counter; in DEV builds promote to `ERR_FAIL_COND_MSG` to surface the first occurrence.

**[maint] streaming_chunk_payload_source.h:94** — `mutable Mutex file_mutex` declared but never locked (zero use sites outside the declaration). Either dead or left over from a path that no longer exists. Invites a future reader to "helpfully" wrap reads in this lock, re-introducing the pack-throughput stall that the comments at `:99-100` and `:168-169` explicitly reversed. — *Fix direction*: delete the member; the existing no-mutex rationale comments stand on their own.

**[maint] streaming_upload_pipeline.cpp:145-163 vs gaussian_streaming.cpp:601-619** — Duplicate `_release_chunk_slot_if_matches` and `_chunk_slot_matches_allocator` in anonymous namespaces across two TUs. Changes must be mirrored; forgetting produces silent ODR-safe-but-inconsistent behavior. Same hygiene gap class as the MSVC PMF trap logged in memory. — *Fix direction*: move both to a shared `streaming_atlas_helpers.h` as `static inline`.

**[perf] streaming_upload_pipeline.cpp:1687-1717** — `clear_pending_uploads` scans *every chunk of every asset* after clearing the queues, looking for orphaned allocator slots or pending states. For 100 assets × 4096 chunks that's 400k iterations with allocator lookups per clear. Fires on every `stop_pack_threads()` and every `process_upload_queue` device-loss early-exit — so a flaky device causes repeated clear+rescan at ms-cost each. — *Fix direction*: maintain a side set of (asset_id, chunk_idx) keys with known pending-or-slot-held state; iterate only that set.

**[perf] streaming_upload_pipeline.cpp:461-468** — Inside the pack_mutex critical section, the dequeue loop computes latency via `_ticks_usec_now()` and calls `telemetry.add_pack_queue_latency()` per job *while holding the mutex*. `pack_mutex` is acquired by queue, peek, consume, requeue, pop, cancel, clear — the hottest lock in this subsystem. Telemetry work has no mutex-protected state. — *Fix direction*: capture `enqueue_usec` inside moved jobs; compute and record latency after the MutexLock scope exits.

**[maint] streaming_atlas.cpp:79-92** — Mixed tab+space indentation inside `_apply_requested_residency`. The function body uses 4-space indent; lines 81-85 and 89-92 use hard tabs where the surrounding block is spaces. Breaks column alignment in every diff viewer and git blame. — *Fix direction*: clang-format the file.

## Cross-cutting patterns

- **Defensive over-validation.** Every upload path re-checks slot ownership, chunk state, and allocator mapping 3-4 times per function. `resolve_upload_chunk` alone runs 6 validators. Response to past bugs; the correct next step is encapsulating the invariants in a state-machine type, not repeating checks at every use site.
- **Public members as implicit friends.** `StreamingUploadPipeline`, `GlobalAtlasState`, `AssetRegistryState`, `AtlasAssetState`, `StreamingChunk` are all public POD with no invariant enforcement. `gaussian_streaming.cpp` reaches into all of them freely.
- **"Force rebuild on detected inconsistency"** substitutes for precondition assertions in `streaming_global_atlas_registry.cpp`. Silent and expensive; hides the originating bug.
- **Duplicated `PackTelemetry` impl.** `streaming_upload_pipeline.h:66-201` has two nearly-identical bodies (SafeNumeric vs no-op) gated on `DEV_ENABLED`. Drift risk.
- **Lambda-heavy god methods.** `process_upload_queue`, `_apply_requested_residency`, `clear_pending_uploads`, `build_cpu_state`, `sync_to_gpu` all rely on nested lambdas that capture the parent frame. Impossible to unit test; fragile under refactor.
- **LIFO allocator vs contiguity-assuming coalescer.** Two adjacent abstractions with incompatible assumptions.

## Recommended refactor moves

### P0 (safety and integrity)

- **P0.1 — Fix `_release_chunk_slot_if_matches` `UINT32_MAX` wildcard** — `streaming_upload_pipeline.cpp:145-154`. Split into `_release_chunk_slot_if_matches` (strict expected) and `_release_chunk_slot_unchecked` (explicit opt-in). Update call sites to use strict variant unless they truly want the wildcard. **Effort: half-day.**
- **P0.2 — Quarantine upload queue across device-loss recovery** — `streaming_upload_pipeline.cpp:606-617`. Add a `uint64_t upload_generation` that is bumped on each `clear_pending_uploads`; pack_thread's `enqueue_upload_job` stamps the current generation; `process_upload_queue` drops any job with a stale generation. Closes the narrow race where a pack worker enqueues onto a just-cleared queue. **Effort: 1 day.**
- **P0.3 — Promote "force rebuild" bailouts to ERR_FAIL in non-production builds** — `streaming_global_atlas_registry.cpp:398-449`. Silent self-heal hides real bugs. **Effort: half-day.**
- **P0.4 — Delete unused `file_mutex`** — `streaming_chunk_payload_source.h:94`. Trap for future contributors. **Effort: minutes.**

### P1 (performance and architecture)

- **P1.1 — Privatize `StreamingUploadPipeline` queue/thread state** — `streaming_upload_pipeline.h:213-272`. Move mutex, queues, read indices, thread pool under `private:`. Expose only `queue_chunk_load`, `process_upload_queue`, `cancel_asset_jobs`, `cancel_chunk_jobs`, `clear_pending_uploads`, `stop_pack_threads`, `has_pending_uploads`, and the depth getters. Most friend access in `gaussian_streaming.cpp` goes away with proper methods. **Effort: 2 days.**
- **P1.2 — FileAccess handle pool for `StagedFileChunkPayloadSource`** — `streaming_chunk_payload_source.cpp:101, :170`. Per-thread LRU cache of open FileAccess refs keyed by file_path. 3-entry cache is plenty. **Effort: 1 day.**
- **P1.3 — Switch atlas slot free list to min-heap** — `streaming_atlas.h:34`. Dense low-slot allocation recovers the coalescing path. `std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<>>` drop-in, with the same API. **Effort: half-day.**
- **P1.4 — Move packed-gaussian checksum verify to DEV_ENABLED only** — `streaming_upload_pipeline.cpp:886`. Pack-thread checksum remains; main-thread re-verify is debug-only. **Effort: hours.**
- **P1.5 — Cap indexed-snapshot range-read** — `streaming_chunk_payload_source.cpp:154-228`. If `max_idx - min_idx > 4 × p_count`, fall back to per-index reads (or sorted-run coalescing). **Effort: 1 day.**

### P2 (maintainability)

- **P2.1 — Extract `UploadDispatcher` from `process_upload_queue`** — `streaming_upload_pipeline.cpp:606-1083`. Name the lambdas; replace shared lambda closures with member fields. **Effort: 3-4 days, with regressions.**
- **P2.2 — Consolidate `_release_chunk_slot_if_matches` / `_chunk_slot_matches_allocator`** — `streaming_upload_pipeline.cpp:145-163`, `gaussian_streaming.cpp:601-619`. Move to a `streaming_atlas_helpers.h` header. **Effort: half-day.**
- **P2.3 — `atlas_published_chunk_count` single source of truth** — `streaming_global_atlas_registry.cpp:290-309`. Either use deltas or use the recount, not both. **Effort: hours.**
- **P2.4 — Clang-format on `streaming_atlas.cpp`** — `streaming_atlas.cpp:78-92`. Tab/space mixing. **Effort: minutes.**
- **P2.5 — Budget-scan cost in `clear_pending_uploads`** — `streaming_upload_pipeline.cpp:1687-1717`. Track the set of chunks-with-allocator-state in a small side-structure; iterate only that. **Effort: 1-2 days.**

## Blind spots

Out-of-slice assumptions the audit takes on faith:

- **`_rollback_pending_chunk` / `_begin_chunk_upload` / `_complete_chunk_load_common`** (`gaussian_streaming.cpp:3115-3161, :3443-3468`). Spot-read, not fully audited against every caller.
- **`GaussianSplatManager::acquire_submission_device` / `ScopedSubmissionLock`.** Upload pipeline assumes correct `buffer_update` serialization across multiple `RenderingDevice` users. When `is_shared_submission_device` returns false the lock is bypassed (`gaussian_streaming.cpp:3244-3246`); unverified.
- **`_resolve_primary_chunk_source_index`** (`queue_chunk_load:552`). A `false` return mid-loop causes `PackJob` early-exit with allocated-but-not-enqueued state; cleanup path would hit the P0.1 wildcard helper.
- **`GaussianData::capture_chunk_snapshot` / `capture_indexed_chunk_snapshot`** thread-safety. Pack workers call these concurrently. If the impl takes an internal mutex, `InMemoryChunkPayloadSource` becomes a pack-thread choke point.
- **Eviction controller** (`streaming_eviction_controller.{h,cpp}`). Touches `atlas_allocator` via `ensure_atlas_slot_available` / `_unload_chunk`; ordering under high churn not audited.
- **Renderer contracts** (`renderer/instance_pipeline_contract.h`, `renderer/pipeline_io_contracts.h`). Assume every `atlas_generation` bump invalidates shader-side caches.
- **`VRAMBudgetRegulator`.** Race-freedom of `budget.vram_usage` updates assumed.
- **`LocalVector::resize` semantics.** The `atlas_published_chunk_count` finding depends on resize preserving existing slots. If that ever changes, the finding escalates from perf to corruption.
- **`Vector<PackedGaussian>` CoW.** Checksum path assumes `packed_data.ptr()` stability between pack and upload.
- **`ProjectSettings` override resolution** (`streaming_upload_pipeline.cpp:31-48`). Tier-cap inheritance depends on current `get_setting_with_override` / `property_get_revert` semantics.

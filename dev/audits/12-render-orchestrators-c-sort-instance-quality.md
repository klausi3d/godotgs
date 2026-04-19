# Render Orchestrators C (Sort/Instance/Quality/Config/Output) — Deep Audit

## Summary

**Grade: C-**

These five orchestrators were extracted from a monolithic `GaussianSplatRenderer` and nominally form the render pipeline's "middle": cull -> sort -> instance -> render -> output. The decomposition is real but **shallow**. Each class "owns" nothing in particular — they are all thin wrappers that mutate shared state structs (`SortingState`, `FrameState`, `PerformanceState`, `DebugState`, `ViewState`, `OutputCompositor::OutputCacheState`, and `GPUCuller::CullingConfig/State`) through a poorly typed provider (`FrameStateProvider`) and a set of hand-rolled "runtime ports" made of C++ **pointer-to-member functions**.

Concrete findings:

1. **Cross-orchestrator state mutation is pervasive.** Sort orchestrator writes `gpu_culler->get_config().cull_params_dirty = false` at five sites (lines 873, 895, 926, 1235, 1345 of `render_sorting_orchestrator.cpp`). Instancing orchestrator writes seven fields of `OutputCompositor::OutputCacheState` directly (lines 15-21, 90, 241-250 of `render_instancing_orchestrator.cpp`). Output orchestrator writes `view_state->manual_viewport_override` and `view_state->active_viewport_color_format`. There is **no contract** — the protocol is "stage N knows which fields belong to stage M and resets them".
2. **Quality orchestrator is a pass-through to `GPUCuller` internals** — 65 direct `gpu_culler->get_config()`/`get_state()` mutations with no clamping invariants beyond trivial `CLAMP()`. It is not an orchestrator; it is a property bag renamed.
3. **Process-wide static HashMap** (`g_fidelity_override_snapshots` at line 29) keyed by `renderer->get_instance_id()`. Leaks on renderer destruction if `preserve_source_fidelity` was not toggled off first. Not thread-safe. See issue #3.
4. **`render_sorting_orchestrator.cpp` is 1,605 LOC with a ~750-line method** `sort_gaussians_for_view` (lines 623-1364) that nests 10+ lambdas. Three nearly identical "write identity indices into GPU buffer" blocks (998-1018, 1052-1069, 1212-1218) exist while a helper `apply_sort_buffer_update` defined at line 1092 is used only once.
5. **MSVC PMF ODR trap in waiting.** `RuntimePorts` (quality/config/output orchestrators) stores `void (GaussianSplatRenderer::*)(...)` with default initializers taking the address of `GaussianSplatRenderer` member functions. `gaussian_splat_renderer.h` forward-declares each orchestrator and includes `gpu_culler.h`, so today it holds — but the pattern is a live time bomb per the MSVC forward-declared-class PMF ODR trap the project already hit (RenderDataOrchestrator, 2026-04-14, see memory log).
6. **No pipeline order enforcement.** Orchestrators are invoked by `RenderPipelineStages` / `GaussianSplatRenderer`, but each can be called independently (e.g. `force_sort_for_view` calls cull + sort from outside a frame). No stage ID, no preconditions on prior-stage output. Contract drift is only caught by sprinkled `ERR_FAIL_*` and index-domain string-comparison checks.

The code works (and the sort orchestrator's fallback policy machinery is genuinely clever), but the architecture promises modularity it does not deliver. A reader has to hold the entire shared state struct graph in their head to understand any one orchestrator.

## What this code does

- **`RenderSortingOrchestrator`** (1,605 LOC cpp): owns the GPU-sort dispatch state machine. Decides between instance-pipeline GPU radix sort, global GPU sort, CPU depth sort, identity-order "published unsorted" fallback, reused-previous-frame fast path, and sort-cache reuse. Owns `InstanceSortCache` (6 fields) and three CPU scratch vectors. Also runs two different sort benchmarks (`run_sort_benchmark`, `benchmark_sorting_performance`) which allocate throwaway GPU buffers and push samples through `GaussianSplatting::compute_sort_benchmark_timing`.
- **`RenderInstancingOrchestrator`** (275 LOC cpp): given N instance transforms, iterates and drives `RenderPipelineStages::execute_cull_stage` -> `execute_sort_stage` -> `render_sorted_splats_with_context` once per instance, then aggregates per-frame metrics (sum of render_time, max of viewport-copy size, averaged frame time via EWMA at α=0.05). Also centralises an instance-pipeline readiness check (`evaluate_instance_pipeline_readiness`).
- **`RenderQualityOrchestrator`** (512 LOC cpp): 21 setters that push into `GPUCuller::CullingConfig`/`State` and `PerformanceSettings`. Holds the "fidelity override" snapshot/restore for the runtime fidelity policy. Owns the GPU cull-pass dispatch itself (`cull_for_view`) — so it is both config-bag and cull-orchestrator.
- **`RenderConfigOrchestrator`** (192 LOC cpp): render-mode / opacity / painterly config setters. Writes four independent config structs (`RenderConfig`, `InteractiveStateConfig`, `CullingConfig`, `PainterlyConfig`). Invalidates cached render through a PMF port when opacity / color grading change. Also hands off interactive-state transitions to `InteractiveStateManager`.
- **`RenderOutputOrchestrator`** (345 LOC cpp): `render_for_view` (standalone "render then copy-to-target" entry point), `copy_final_texture_to_target`, `commit_to_render_buffers` (which, per the in-code comment, now **does nothing** for compositing — it only clears pending flags).

Every orchestrator has an `explicit Orchestrator(const Dependencies&)` ctor and `ERR_FAIL_NULL` guards. All of them end with a block of `GaussianSplatRenderer::foo(...) { my_orchestrator->foo(...); }` trampolines — 38 of them in `render_sorting_orchestrator.cpp` alone (grep count of orchestrator identifiers: 38 in `gaussian_splat_renderer.cpp`, 24 in quality, 16 in config, 11 in sort, 8 in output).

## Orchestrator interaction map

Direct orchestrator-to-orchestrator calls: **none.** They all route through `GaussianSplatRenderer` trampolines or share state structs. So the actual coupling is via **shared mutable state**:

```
                 GaussianSplatRenderer  (state-struct container)
                 ├── SortingState        written by Sort, read by Instance
                 ├── FrameState          written by Sort, Instance, Output, Quality
                 ├── PerformanceState    written by Sort, Instance, Quality (metrics)
                 ├── DebugState          written by Sort (sort_route_uid), Instance (last_stage_metrics)
                 ├── ViewState           written by Output (direct), read by Sort
                 ├── ResourceState       read by Output (gpu_resources_initialized), mutated via deletion_queue by Sort
                 └── SubsystemState      holds refs to output_compositor, painterly_renderer

  GPUCuller::CullingConfig / CullingState
    written by: Quality (65 refs) — primary owner
                Sort    (18 refs) — especially cull_params_dirty = false at 5 sites
                Output  ( 1 ref ) — last_cull_viewport_size = viewport_size

  OutputCompositor::OutputCacheState
    written by: Output   (via copy_to_render_target)
                Instance (7 fields rewritten per-frame, see lines 15-21 + 241-250)
                Sort     (no direct writes; reads has_valid_render through snapshot path)
```

Who calls/mutates whom:

| Stage   | Writes into SortingState | Writes into CullingConfig/State | Writes into OutputCacheState | Writes into ViewState | Reads from Sorting |
|---------|:------------------------:|:-------------------------------:|:----------------------------:|:---------------------:|:------------------:|
| Sort    | primary                  | yes (cull_params_dirty, cache flags) | no                      | reads only            | primary            |
| Instance| yes (sort counts reset) | no                              | yes (7 fields)               | no                    | yes (snapshot)     |
| Quality | yes (sorter_needs_rebuild via max_splats) | primary                | no                           | no                    | no                 |
| Config  | no                       | no                              | no (invalidates via PMF)     | no                    | no                 |
| Output  | no                       | yes (last_cull_viewport_size)   | indirect (via compositor)    | yes                   | no                 |

**Pipeline order** (as actually invoked by `RenderInstancingOrchestrator::render_instanced`):
`Quality.cull_for_view -> Sort.sort_gaussians_for_view -> PipelineStages.render_sorted_splats_with_context -> Output.commit/copy`

This order is not encoded anywhere. Sort's `force_sort_for_view` (line 1366) demonstrates the fragility: it has to re-do streaming orchestration, re-invoke `cull_for_view`, then sort, because nothing else guarantees the sequencing.

**Blind spot: `gpu_sorter.cpp` (3,413 LOC, Unit 14).** The orchestrator does not duplicate low-level sort kernels — `fill_sort_padding`, `sort_async`, `compute_sort_benchmark_timing` live in `gpu_sorter.cpp` / `sort_benchmark_metrics.*`, and `GPUSortingPipeline` is the abstraction the orchestrator uses (`sort_gaussians_gpu`, `rebuild_sorter_if_needed`, `ensure_sort_buffers`). What *is* duplicated is the sort-buffer-write code path (see issue #4).

## Strengths

- **Pipeline-stage separation of concerns is partially real.** Instance readiness is centralised in one static function (`evaluate_instance_pipeline_readiness`, `render_instancing_orchestrator.cpp:39`) with a clear `InstanceReadinessResult` / `InstanceReadinessFailureMode` enum rather than 3 booleans.
- **Sort fallback policy is table-driven.** The `SortFallbackPolicyDecision` / `execute_sort_fallback_policy` lambda (`render_sorting_orchestrator.cpp:1254`) dispatches a small action list (REUSE_PREVIOUS_SORT, PUBLISH_INSTANCE_IDENTITY, RUN_CPU_SORT, FAIL). That is a much nicer pattern than the 5-layer if/else it replaced.
- **Sorter-init backoff** (`render_sorting_orchestrator.cpp:270-285, 312-332`) prevents a GPU-OOM crash spiral: after `kSorterInitMaxFailures` failed inits, GPU sort is permanently disabled for the session. This is the right pattern.
- **Strict-global-sort correctness guard** (line 1076-1083): strict mode refuses to publish approximate ordering and forces a real sort instead, rather than silently rendering wrong output.
- **Dependency injection with callbacks.** `RenderSortingOrchestrator::Dependencies` (h:22-32) takes `std::function<>` for `cull_for_view`, `record_rendering_error`, `ensure_rendering_device`. This is a real seam — not great, but better than direct renderer method calls.
- **Constructor precondition checks.** Every orchestrator `ERR_FAIL_NULL`s its dependencies and `ERR_FAIL_COND_MSG`s its callbacks. Hard to misuse at setup.
- **Cull stage output has a visible_domain contract.** `IndexDomain` (CHUNK_REF, SPLAT_REF, GAUSSIAN_GLOBAL, UNKNOWN) is checked at sort-stage entry (lines 734-757) and violations are hard errors. This is the closest thing to a typed pipeline contract in the slice.
- **Sort benchmark timing uses a dedicated helper** (`GaussianSplatting::compute_sort_benchmark_timing`, `sort_benchmark_metrics.*`) rather than re-deriving `used_async / async_requested / waited_for_completion` flags inline.

## Top issues

1. **[severity: corruption]** `render_sorting_orchestrator.cpp:873,895,926,1235,1345` — Sort orchestrator mutates `gpu_culler->get_config().cull_params_dirty = false` in 5 places. This flag is owned by the cull stage (GPUCuller), so sort is reaching across the stage boundary to signal "I consumed cull params". Any new sort path that forgets this line causes a sort-skip loop (camera stable, cull dirty not cleared, re-sort every frame); any new cull path that also resets the flag creates a race. — Why it matters: silent perf cliff, and the coupling is invisible from signatures. — Fix: make "cull-consumed" an output of the sort stage or add a `consume_cull_dirty()` method on GPUCuller; or track dirty on SortingState, not CullingConfig.

2. **[severity: corruption]** `render_instancing_orchestrator.cpp:15-21, 90, 241-250` — Instancing orchestrator directly writes 7 fields of `OutputCompositor::OutputCacheState` (including `last_viewport_copy_success`, `has_valid_render`). The output orchestrator *also* owns these (via the compositor). Any single-instance render that fails readiness resets fields the compositor just wrote. — Why it matters: flicker / stale size metrics exported to debug overlays; breaks the `was_last_viewport_copy_successful()` contract when readiness fails mid-stream. — Fix: push a `instance_pipeline_not_ready()` method on OutputCompositor that atomically clears *its own* state; do not cross the boundary.

3. **[severity: crash]** `render_quality_orchestrator.cpp:29` — `static HashMap<uint64_t, FidelityOverrideSnapshot> g_fidelity_override_snapshots` is process-wide and never cleaned when a `GaussianSplatRenderer` is destroyed. If `preserve_source_fidelity` flipped true during a renderer's life, the snapshot stays in the map forever. Also not thread-safe (HashMap + multiple render threads possible). — Why it matters: slow memory leak over a session; if a recycled instance_id ever collides, restores wrong snapshot. — Fix: move snapshot onto `RenderQualityOrchestrator` as a member `std::optional<FidelityOverrideSnapshot>`; delete the global. Dtor then correctly bounds lifetime.

4. **[severity: maint]** `render_sorting_orchestrator.cpp:998-1018, 1052-1069, 1212-1218` — Three almost-identical blocks that resize `sorting_state.sort_index_bytes`, fill it with sequential or culled indices, and `buffer_update` into the GPU sort-indices buffer. The helper `apply_sort_buffer_update` defined at line 1092 does exactly the last step but is used **once** (line 1218). — Why it matters: any fix to GPU buffer-update sync semantics has to be made in three places. — Fix: extend `apply_sort_buffer_update` to take a filler lambda or an input indices pointer, use it from all three sites.

5. **[severity: maint]** `render_sorting_orchestrator.cpp:623-1364` — Single method `sort_gaussians_for_view` is ~750 lines with 10 nested lambdas (`resolve_output_domain_for_input`, `build_summary`, `set_active_sort_algorithm`, `compute_current_cull_signature`, `refresh_cull_signature_tracking`, `reset_sort_metrics`, `record_gpu_sort_sample`, `reuse_previous_sort`, `compute_instance_visible_splat_budget`, `publish_instance_identity_fallback`, `apply_sort_buffer_update`, `record_cpu_sort_sample`, `run_cpu_sort`, `execute_sort_fallback_policy`). Each lambda closes over 15+ stack variables. — Why it matters: impossible to unit-test any branch in isolation; reviewers cannot hold this in working memory. — Fix: extract state into a `SortDispatchContext` struct and make the lambdas free functions taking it by reference. Separate the "decide route" phase from the "execute route" phase.

6. **[severity: perf]** `render_sorting_orchestrator.cpp:303, 671, 678` — `SortingStrategyConfig::load_from_project_settings()` is called once per `refresh_gpu_sorter` *and* once per `sort_gaussians_for_view`, every frame. `set_forced_sort_algorithm` is pushed to `sorting_pipeline` at lines 304 and 678. Project settings reads go through `ProjectSettings::get_singleton()->get_setting` — not free at 60 Hz. — Why it matters: 1-2% CPU on settings plumbing per frame, repeated redundantly. — Fix: cache `SortingStrategyConfig` on the orchestrator, invalidate on `ProjectSettings::settings_changed` signal.

7. **[severity: corruption]** `render_sorting_orchestrator.cpp:176-178` — Comment says "Use buffer capacity instead of stale async readback. GPU-side counter drives actual dispatch; this is a structural guard only." The code sets `visible_chunk_count = buffers.max_visible_chunks` — i.e. **always the capacity, never the live count**. That means `_sync_instance_sort_inputs` passes the *maximum* to the sort pipeline even when only 3 chunks are visible. Downstream correctness depends on the GPU-side counter overriding this. — Why it matters: any path that trusts the host-side `visible_chunk_count` (e.g. diagnostics, fallback sizing, `compute_instance_visible_splat_budget` at line 840) reads a lie. `compute_instance_visible_splat_budget` indeed uses it at line 844 and will overestimate the budget, potentially letting stale tails pass the size guard at line 864. — Fix: either propagate live count through a GPU-updated readback (with stale-frame tolerance encoded), or rename the parameter to `chunk_count_capacity` and audit every consumer.

8. **[severity: maint]** `render_quality_orchestrator.h:13-23` — `RuntimePorts` stores pointer-to-member-functions as defaulted struct members with `static_cast<...>` disambiguators for overloaded getters. This matches the project-wide MSVC PMF ODR trap (see memory log, 2026-04-14). Today `gaussian_splat_renderer.h` is fully included, so it holds — but nothing stops a future refactor from swapping the include for a forward declaration, at which point the PMF is silently the wrong size on one TU and produces heap corruption. — Why it matters: one-line refactor, silent heap damage. — Fix: replace PMF runtime_ports with `std::function<>` callbacks (like sort orchestrator already does) or with interface pointers. The cost is one indirection per call for code that is not on the hot path.

9. **[severity: perf]** `render_sorting_orchestrator.cpp:762-778` — `compute_current_cull_signature` is a per-element FNV hash of `cull_state.culled_indices`. For 1M visible splats on a camera-stable frame, that's 1M XOR+multiply — to decide whether to skip a sort. It's memoised per frame (`current_cull_signature_computed`), but the signature itself is computed unconditionally inside `can_validate_previous_global_sort`. — Why it matters: sort-skip fast path costs O(N) whether it triggers or not, partly defeating the optimisation. — Fix: maintain the signature incrementally in the cull stage (cheap — already iterating the array), or use a monotonic generation counter from GPUCuller as a proxy.

10. **[severity: corruption]** `render_output_orchestrator.cpp:133, 177` — `view_state->manual_viewport_override = viewport_size` then `view_state->manual_viewport_override = Size2i()` around the render_gaussians call. If `render_gaussians` early-returns or throws (err macros don't throw but err paths abort), the override is never reset. Sort orchestrator reads this at `render_sorting_orchestrator.cpp:149`. — Why it matters: stuck manual viewport override on the next frame -> wrong cull viewport -> wrong visibility. — Fix: RAII guard on ViewState manual override, not manual save/restore.

11. **[severity: maint]** `render_quality_orchestrator.cpp:287-305` — `set_quality_preset` hard-codes three presets with magic numbers (1M / 500k / 250k max_splats, LOD bias 0.8/1.0/1.5). `get_quality_preset` at lines 312-323 tries to *infer* the current preset from `performance_settings.max_splats` but uses completely different thresholds (10M / 5M / 2M) and doesn't reference the same constants. So `set_quality_preset("quality")` followed by `get_quality_preset()` returns `"low"`. — Why it matters: configurator UIs will churn; tests can't round-trip. — Fix: store the preset enum directly; derive numerical settings from it.

12. **[severity: crash]** `render_sorting_orchestrator.cpp:1009-1016` — Identity fallback path reads `renderer->get_resource_owner(sort_indices_buffer, renderer->get_device_state().rd)`. `renderer->get_device_state().rd` is the root `RenderingDevice*`. If the sort-indices buffer was created on a compute-local device that has since been destroyed, `get_resource_owner` returns `nullptr` and the code falls through to `target_device = renderer->get_device_state().rd` — which is a *different* device. `buffer_update` on an RID that does not belong to that device is undefined behaviour in Godot's RD. Same pattern at lines 1061-1063 and 1102-1104. — Why it matters: heisen-bugs under device-loss or multi-device scenarios; the fallback path is exercised rarely so the bug hides. — Fix: skip the update if `get_resource_owner` returns nullptr rather than defaulting to the renderer's root device.

13. **[severity: maint]** `render_instancing_orchestrator.cpp:141-149, 228-233` — Multi-instance branch repeatedly saves and restores `frame_counter`, `total_frames_rendered`, `avg_frame_time_ms`, `peak_frame_time_ms` around each inner render to prevent per-instance double-counting, then does one aggregated EWMA update at the end (line 253-266). The save/restore is fragile — any new metric added to `PerformanceMetrics` that is incremented per-frame will be double-counted until someone remembers to save/restore it here. — Why it matters: silent metrics drift, hard to detect. — Fix: introduce a `FrameMetricsScope` RAII guard, or render to a throwaway aggregation context and commit once.

14. **[severity: maint]** `render_config_orchestrator.cpp:3-4, 88-90` — `RenderConfigOrchestrator::set_painterly_enabled` reaches into `PainterlyRenderer->get_pass_graph()->reset()` directly. If painterly reset semantics ever change (e.g. must drain a command queue first), config orchestrator has no way to know. — Why it matters: breaking change contagion; config shouldn't know about pass-graph internals. — Fix: `PainterlyRenderer::notify_disabled()` method that encapsulates the reset.

15. **[severity: perf]** `render_output_orchestrator.cpp:33, 87, 100, 223, 234, 243, 254` — Seven separate `GaussianSplatRenderer::FrameStateProvider` constructions in a 345-line file. Each is a small struct (cheap) but each call into e.g. `get_output_compositor()` goes through an indirection chain. Six of these are const-view-only and could share one instance per entry point. Same pattern in `render_sorting_orchestrator.cpp` (6 constructions) and `render_instancing_orchestrator.cpp` (2 constructions plus one per instance iteration at line 156). — Why it matters: negligible hot-path cost, but the pattern implies the API is too cheap to cache, which encourages re-fetching stale state. — Fix: accept `IFrameStateView&` / `IFrameMutationAccess&` as a method parameter and make the caller responsible for lifetime.

## Cross-cutting patterns

- **"FrameStateProvider" is an anti-abstraction.** It is both view and mutator, cheaply constructed, and read-anywhere-mutate-anywhere. Every orchestrator constructs one at the top of most methods. It documents *nothing* — no read/write set, no locking, no ordering. It is a typed `this`-pointer with extra steps.
- **Orchestrators are anemic.** Quality has 21 setters that do `clamp; if_changed; set_dirty`. Config has 15 similar. These are not orchestrators; they are Java bean validators. The only methods that do real work are `cull_for_view` (Quality), `sort_gaussians_for_view` (Sort), `render_instanced` (Instance), `render_for_view` (Output). One actual method per class — the other methods are scope pollution.
- **Runtime ports via PMF vs via std::function is inconsistent.** Sort uses `std::function<>` (h:18-20); Quality/Config/Output use PMF structs with defaulted member initialisers. No rationale in comments for the split. The PMF variant is faster (one indirection), incompatible with lambdas, and is the known ODR trap.
- **The "sort stage" owns four pipelines at once.** Instance GPU sort, global GPU sort, CPU depth sort, identity-order fallback, *and* sort-cache reuse. Each has separate correctness preconditions, metric recording, and `RenderRouteUID` tagging. The single `sort_gaussians_for_view` method is the integration point. It should be split: `InstancePipelineSorter`, `GlobalPipelineSorter`, `CpuDepthSorter`, with `RenderSortingOrchestrator` dispatching by domain.
- **Metrics are written from 4+ orchestrators.** `performance_state.metrics.sort_*`, `rendered_splat_count`, `culling_*` are updated from Sort, Instance, Quality. There's no single owner, no atomic "frame committed" boundary. The EWMA save/restore dance in instancing (#13) is the symptom.
- **`GaussianSplatRenderer::foo() { orchestrator->foo(); }` trampolines.** Every public API surface is doubled: once on the renderer, once on the orchestrator. Render-thread dispatch guards (e.g. `_dispatch_call_on_render_thread_blocking` in `set_max_splats`) sit on the renderer side — so if you call `quality_orchestrator->set_max_splats` directly from a test, you bypass the render-thread check silently. This is a sharp edge waiting for someone to test the orchestrator in isolation.
- **Strict / non-strict modes are ambient.** `g_gpu_sorting_config.strict_global_sort` is read in sort orchestrator at lines 673, 822, 1076, 1162, 1258 and changes behaviour in ~7 branches. It is a global. The orchestrator is not a function of its arguments.

## Recommended refactor moves

**P0 (correctness, do now, ~1 week):**

- Fix issue #3 (process-wide `g_fidelity_override_snapshots`) — move to member, trivial. 2 hours.
- Fix issue #12 (fallback-path `get_resource_owner` returning nullptr) — add null check, skip update. 1 hour.
- Fix issue #7 (`visible_chunk_count = max_visible_chunks` lie) — rename variable + audit `compute_instance_visible_splat_budget` size guard. 1 day.
- Fix issue #11 (set/get preset round-trip broken) — store preset enum. 2 hours.

**P1 (coupling, next sprint, ~2 weeks):**

- Replace PMF runtime_ports with `std::function<>` everywhere (issue #8). Uniformity with sort orchestrator, closes the MSVC ODR trap door. 1 day.
- Extract `cull_params_dirty` consumption into a formal `SortStageOutput::consumed_cull_dirty = true` flag on the SortStageOutput struct and have `RenderPipelineStages` or `GPUCuller` apply it — sort orchestrator should not reach into `gpu_culler->get_config()` at all (issue #1). 2-3 days.
- Add a `OutputCompositor::note_instance_not_ready()` method and route instancing orchestrator's cache resets through it (issue #2). Half a day.
- Consolidate the three identity-fill blocks in sort orchestrator behind `apply_sort_buffer_update` (issue #4). Half a day.
- RAII `FrameMetricsScope` for multi-instance (issue #13). 1 day.

**P2 (structural, when architecture thaws, ~4 weeks):**

- Split `sort_gaussians_for_view` by index domain: `InstanceSortDispatcher`, `GlobalSortDispatcher`, `CpuSortDispatcher`, each ~300 LOC, plus a ~150 LOC `RenderSortingOrchestrator` that picks which dispatcher. Makes each dispatcher unit-testable. (Issue #5 + issue #4 together.) 1 week.
- Rename `RenderQualityOrchestrator` to `RenderQualityConfigurator` and pull `cull_for_view` out into a dedicated `GpuCullDispatcher`. The current class is two things sharing a constructor. 2-3 days.
- Replace `FrameStateProvider` with per-stage typed view/mutation interfaces (`ISortStageMutation`, `ICullStageMutation`) that expose only the fields that stage owns. Compiler enforces ownership. 2 weeks.
- Cache `SortingStrategyConfig` on orchestrator, subscribe to `ProjectSettings::settings_changed` (issue #6). Half a day once config plumbing exists.
- Make `strict_global_sort` a field on `SortingStrategyConfig` read once per frame, not a global (cross-cutting global cleanup). 1 day.

Effort summary: ~5-6 weeks engineer time to go from C- to B+. The P0 set alone closes the most dangerous known issues in under a week.

## Blind spots

- **`gpu_sorter.cpp` (3,413 LOC, Unit 14)** — the low-level radix/bitonic/one-sweep kernels. The orchestrator uses `GPUSortingPipeline` / `IGPUSorter` as the abstraction; no low-level sort logic is duplicated in the orchestrator. Benchmark helpers (`fill_sort_padding`, `compute_sort_benchmark_timing`) are in shared headers, not duplicated. **Not a duplication concern.**
- **`render_pipeline_stages.cpp`** — actually performs `execute_cull_stage` / `execute_sort_stage` / `render_sorted_splats_with_context`. Instancing calls into these directly (line 182, 193, 215). The contract between PipelineStages and these orchestrators is out-of-slice.
- **`gpu_sorting_pipeline.cpp`** — `sort_gaussians_gpu`, `sort_result_sink`, `set_instance_pipeline_inputs`. Most of the instance-pipeline sort correctness lives here. Orchestrator is a caller.
- **`render_streaming_orchestrator`** — referenced from `force_sort_for_view` (line 1389-1411). The "ensure streaming system, bootstrap, then sort" dance is cross-unit.
- **`gaussian_splat_renderer.cpp`** — ~38 trampoline implementations of orchestrator-forwarded APIs plus the render-thread dispatch wrappers (`_dispatch_call_on_render_thread_blocking`). The orchestrator is only half the public surface; the other half (thread safety) is on the renderer.
- **`FrameStateProvider` / `IFrameStateView` / `IFrameMutationAccess` definitions** — live in `gaussian_splat_renderer.h` and `render_types/render_state_types.h`. The interface between orchestrators and shared state is entirely defined out-of-slice.
- **`SortingState`, `FrameState`, `PerformanceState`, `DebugState` struct definitions** — all out-of-slice. The orchestrators dereference 30+ fields across these; their invariants are not local.
- **`InstancePipelineContract`** — used at sort line 169, instancing line 51-53. The contract's own soundness (what "has_sort_buffers" means) is out-of-slice.
- **`OutputCompositor::OutputCacheState`** — instancing writes directly into it at 7 sites; the state's invariants are defined in `output_compositor.h` (out-of-slice).
- **Render-thread dispatch path for `set_max_splats` / `force_sort_for_view`** — implemented on the renderer (`gaussian_splat_renderer.cpp:1417+`), not the orchestrator. Tests targeting the orchestrator directly bypass the render-thread protection.
- **`RenderRouteUID` string constants** — referenced everywhere in sort orchestrator (`COMMON_SKIP_*`, `INSTANCE_SORT_*`) but defined in `render_debug_state_orchestrator.h` (out-of-slice for this unit). Their lifetime and uniqueness guarantees are not audited here.

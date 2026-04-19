# Render Orchestrators A (Device/Resource/Data) — Deep Audit

Scope: `modules/gaussian_splatting/renderer/render_device_orchestrator.{h,cpp}`,
`render_resource_orchestrator.{h,cpp}`, `render_data_orchestrator.{h,cpp}`,
`render_frame_context_manager.{h,cpp}`. ~2,000 LOC.

Reviewer lens: ownership graphs, destructor order, dependency injection bag
smell, cyclic back-pointer hazards.

## Summary

Grade: **D+**.

The extraction is cosmetic. Every one of these "orchestrators" is a raw
back-pointer (`GaussianSplatRenderer *renderer`) with a fistful of raw sub-state
pointers bolted on. They are *not* independent units — they re-enter the
renderer on every call via `FrameStateProvider(renderer)`, call pointer-to-
member-function runtime ports on `renderer`, and share mutable state structs by
raw pointer. The "Dependencies struct with 11 pointers + RuntimePorts" red flag
called out by the prior audit was not just real — it was already a known MSVC
ODR trap that silently corrupted the heap, patched with a 10-line comment
(`render_data_orchestrator.h:8-16`) instead of a structural fix. `DeviceState`
lives inside `RenderDeviceOrchestrator` but the renderer hands out a raw
reference to it through `get_device_state()`, and when `data_orchestrator` is
null the getter returns a mutable reference to a **process-wide `static`
fallback** (`gaussian_splat_renderer.cpp:2517-2527`) — hidden global state that
cross-contaminates between renderer instances. The code ships today only
because the declaration order in `gaussian_splat_renderer.h` happens to keep
`subsystem_state` alive past orchestrator destruction; no invariant enforces
it.

This is not architecture. This is a god-object split into a twelve-member
constellation of pointers that all still point at the same god-object.

## What this code does

- `RenderDeviceOrchestrator` — owns `DeviceState` (`rd`, missing-device flags),
  proxies every RenderingDevice/submission-device acquisition, and delegates
  RID-ownership tracking to `RenderDeviceManager`. Also synchronizes tile
  submissions and records cross-device operations via three
  `std::function` callbacks into `RenderDiagnosticsOrchestrator`.
  (`render_device_orchestrator.cpp:78-465`)
- `RenderResourceOrchestrator` — owns `PipelineState` + `ResourceState`,
  initializes shaders, creates GPU buffers (test data, instance, painterly,
  tile renderer, interactive state manager), and runs pipeline-feature
  capability checks. Calls back into the renderer via six pointer-to-member
  runtime ports. (`render_resource_orchestrator.cpp:120-480`, `render_resource_orchestrator.h:8-15`)
- `RenderDataOrchestrator` — owns `SceneState`, `StreamingState`, and
  `GaussianStreamingSystem::ConfigOverrides`. Accepts `set_gaussian_data`,
  `set_gaussian_asset`, `set_static_chunks`, `set_streaming_config_overrides`,
  and performs the GPU-buffer streaming initialization for real data.
  Drives state mutation through `FrameStateProvider(renderer)` — which is
  a proxy that reaches back through the very renderer that owns this
  orchestrator. (`render_data_orchestrator.cpp:51-417`)
- `RenderFrameContextManager` — a dumb POD container that groups `FrameState`
  (an atomic splat count, frame counter, two timing floats) and `ViewState`
  (last camera transform/projection/position + viewport overrides). Thirty
  lines of state + two reset functions. (`render_frame_context_manager.h:12-45`)

## Ownership graph

### Declared ownership (what the types say)

```
GaussianSplatRenderer                            (singleton per 3D world)
├── SubsystemState subsystem_state               (value, declared line 577)
│   ├── Ref<RenderDeviceManager> device_manager
│   ├── Ref<GPUSortingPipeline>  sorting_pipeline
│   └── Ref<…9 others…>
├── RenderFrameContextManager frame_context_manager  (value, line 298)
│   ├── FrameState  { atomic<uint32> visible_splat_count, frame_counter, 2×float }
│   └── ViewState   { Transform3D, Projection, Vector3, 2× Size2i, 2× DataFormat, bool }
├── std::unique_ptr<RenderDeviceOrchestrator>   (line 580)
│   ├── DeviceState device_state                (by value — auth. copy)
│   ├── GaussianSplatRenderer *renderer         (back-pointer)
│   ├── RenderDeviceManager *device_manager     (raw; alias of subsystem_state.device_manager.ptr())
│   ├── GPUSortingPipeline *sorting_pipeline    (raw; alias of subsystem_state.sorting_pipeline.ptr())
│   └── 3× std::function<…> callbacks           (capture [this]=renderer → diagnostics_orchestrator)
├── std::unique_ptr<RenderResourceOrchestrator> (line 588)
│   ├── PipelineState pipeline_state            (by value — auth. copy)
│   ├── ResourceState resource_state            (by value — auth. copy)
│   ├── GaussianSplatRenderer *renderer
│   ├── DeviceState *device_state               (raw alias → device_orchestrator->device_state)
│   ├── PerformanceSettings *                   (raw alias → renderer member)
│   ├── PainterlyConfig *                       (raw alias)
│   ├── DebugConfig *                           (raw alias)
│   ├── TestDataState *                         (raw alias → renderer->test_data_state)
│   ├── TileRendererState *                     (raw alias → renderer->tile_renderer_state)
│   ├── SubsystemState *                        (raw alias → renderer->subsystem_state)
│   ├── PipelineFeatureSet *                    (raw alias → renderer->pipeline_features_effective)
│   ├── String *                                (raw alias → renderer->pipeline_features_warning_cache)
│   └── RuntimePorts { 6× PMF into GaussianSplatRenderer }
├── std::unique_ptr<RenderDataOrchestrator>    (line 589)
│   ├── SceneState scene_state                  (by value — auth. copy)
│   ├── StreamingState streaming_state          (by value — auth. copy)
│   ├── ConfigOverrides streaming_config_overrides
│   ├── GaussianSplatRenderer *renderer
│   ├── const DebugConfig * / PerformanceSettings * / GPUCuller::CullingConfig *
│   ├── 3× std::function<…> callbacks           (capture [this]=renderer)
│   └── RuntimePorts { 1× PMF into GaussianSplatRenderer }
└── std::unique_ptr<…10 other orchestrators…>
```

### Effective ownership (what actually runs)

The only real owner is `GaussianSplatRenderer`. Every orchestrator is a
functional façade whose "state" is logically still the renderer's — it is moved
physically into the orchestrator but then handed back out to the renderer via
`get_device_state()`/`get_pipeline_state()`/`get_streaming_state()` accessors.
The renderer *itself* writes to these fields during `_teardown_resources()`
(e.g. `get_resource_state().deletion_queue.flush_all()` at
`gaussian_splat_renderer.cpp:1177`, `get_pipeline_state().gaussian_shader = RID()`
at line 1270, `get_streaming_state().current_streaming_system.unref()` at
line 1219). So the split does not localize mutation — it just interposes a
reference dereference.

### Back-pointer graph (the cycle)

```
renderer --owns--> device_orchestrator --has--> renderer (raw ptr)
                                       --has--> diagnostics_orchestrator via std::function [this=renderer]
renderer --owns--> resource_orchestrator --has--> renderer (raw ptr + 6 PMFs on renderer)
                                         --has--> device_state (raw ptr INTO device_orchestrator)
                                         --has--> subsystem_state (raw ptr into renderer)
renderer --owns--> data_orchestrator --has--> renderer (raw ptr + 1 PMF)
                                     --has--> renderer.get_subsystem_state().device_manager
                                              (through state_view)
```

Every orchestrator holds `renderer`. Every orchestrator's lambdas capture
`[this]` (the renderer). `resource_orchestrator` additionally holds a raw
pointer *into* `device_orchestrator`'s member (`device_state`) — a
cross-orchestrator back-edge that the lifetime of neither type enforces.

### Destructor order

The renderer's automatic destruction order is reverse declaration order in
`gaussian_splat_renderer.h`:

1. `render_thread_dispatcher` (line 591)
2. `output_orchestrator`
3. `data_orchestrator`
4. `resource_orchestrator`
5. `instancing_orchestrator`
6. `config_orchestrator`
7. `quality_orchestrator`
8. `streaming_orchestrator`
9. `sorting_orchestrator`
10. `diagnostics_orchestrator`
11. `debug_state_orchestrator`
12. `device_orchestrator`
13. `pipeline_stages` (line 579)
14. … then `subsystem_state` (line 577) — `Ref<>`s released
15. … then `frame_context_manager` (line 298)

This order is **silently correct**: `device_orchestrator` holds a raw pointer
to `subsystem_state.device_manager` and the Ref<> is only unref'd *after*
`device_orchestrator` is gone. But:

- **Nothing in the type system guarantees this.** Move
  `SubsystemState subsystem_state` below the unique_ptr block and the code
  compiles, links, starts, renders for minutes, and then segfaults in
  `~RenderDeviceOrchestrator` (there is none today — but if one is added that
  touches `device_manager`, you eat dangling ptr).
- `resource_orchestrator::device_state` aliases
  `device_orchestrator->device_state`. `resource_orchestrator` destructs
  **before** `device_orchestrator`, so ordering is accidentally safe.
  If the declaration order in `gaussian_splat_renderer.h:580-589` is
  reshuffled by a tidy-minded refactor, `resource_orchestrator` could destruct
  after `device_orchestrator` and hold a dangling pointer.
- `_teardown_resources()` runs **before** any unique_ptr destructs
  (it's called from `~GaussianSplatRenderer`). That function unrefs
  `subsystem_state.device_manager` at line 1299 and then *returns* — the
  unique_ptrs destroy after that. If any orchestrator destructor ever calls
  `device_manager->…`, it hits a null Ref.

**Verdict:** destructor order is a tripwire, not a contract.

## Strengths

- `RenderFrameContextManager` is the one genuinely well-scoped unit.
  Small, owns by value, no back-pointer, trivial to reason about.
  This is what the other three *should* look like.
  (`render_frame_context_manager.h:1-47`)
- Constructors assert all required ports are non-null with
  `ERR_FAIL_NULL` / `ERR_FAIL_COND_MSG` — precondition-checked DI.
  (`render_resource_orchestrator.cpp:26-41`, `render_data_orchestrator.cpp:27-34`,
  `render_device_orchestrator.cpp:87-91`)
- Known MSVC PMF ODR trap is documented in the header with a useful
  explanation of the failure mode (24-byte vs 8-byte PMF, 8-byte heap
  overflow).  That comment is load-bearing — future contributors need
  it. (`render_data_orchestrator.h:6-16`)
- `RenderDeviceOrchestrator::get_texture_format` tries multiple candidate
  devices in a de-duplicated list — robust against cross-device texture
  ownership. (`render_device_orchestrator.cpp:243-286`)
- `safe_submit_sync` and `synchronize_tile_submission` carefully refuse to
  submit/sync on the main RenderingDevice (Godot owns its frame lifecycle) —
  this is correct discipline. (`render_device_orchestrator.cpp:140-143`,
  `render_device_orchestrator.cpp:447-449`)

## Top issues

1. **[maint]** `render_resource_orchestrator.h:17-29` — `Dependencies` struct
   carries 10 raw pointers + a `RuntimePorts` struct of 6 PMFs, all into the
   same `GaussianSplatRenderer`. The prior audit's "god-object smell" is
   confirmed. Every added field in `GaussianSplatRenderer` that this
   orchestrator needs becomes a new line in `Dependencies`, a new line in
   the member list, a new line in the ctor init list, and a new
   `ERR_FAIL_NULL`. The cost of change is linear in the size of the bag.
   — *Fix:* replace the bag with `GaussianSplatRenderer &renderer` and typed
   accessor interfaces on the renderer (e.g. `IResourceHost`) that expose
   exactly what the orchestrator needs. Eleven pointers collapse to one.

2. **[corruption]** `gaussian_splat_renderer.cpp:2517-2538` — `get_streaming_state()`,
   `get_resource_state()`, `get_pipeline_state()`, `get_scene_state()`,
   `get_device_state()` all return `static …Fallback;` if the corresponding
   orchestrator is null. This is **process-wide mutable state** masquerading
   as a per-renderer accessor. Two renderers with null orchestrators share
   the same fallback; a caller who holds the returned reference across
   orchestrator reinitialization gets an object whose mutations silently
   land in a global. — *Why it matters:* classic data-race and cross-instance
   contamination bug. The atomic `visible_splat_count` inside the fallback
   `StreamingState → FrameState` (if any chain hits) is shared across
   renderers. — *Fix:* make getters return pointers (`T *`, `nullptr` on
   failure) or throw; callers check. Delete the static fallback pattern.

3. **[crash]** `gaussian_splat_renderer.h:577-591` — destructor ordering
   invariant is implicit. `RenderDeviceOrchestrator::device_manager` is a
   raw alias of `subsystem_state.device_manager.ptr()`;
   `RenderResourceOrchestrator::device_state` is a raw alias of
   `device_orchestrator->device_state`. Both rely on
   reverse-declaration-order safety that a reshuffle will break silently.
   The code will compile, run for minutes, and crash in teardown on some
   platforms only. — *Fix:* at minimum, add a `static_assert` / comment
   block near the member declarations asserting the ordering contract.
   Long-term: stop storing aliases; re-fetch on demand via an accessor.

4. **[maint/corruption]** `render_resource_orchestrator.h:8-15` +
   `render_data_orchestrator.h:27-29` — `RuntimePorts` is pointer-to-member-
   function into `GaussianSplatRenderer`. This is the exact construct that
   already produced an 8-byte heap overflow on MSVC
   (documented at `render_data_orchestrator.h:8-16`, fixed by adding
   `#include "gaussian_splat_renderer.h"`). The same trap lurks in every
   orchestrator header that uses PMFs. Any header that forward-declares
   `GaussianSplatRenderer` and then stores a PMF will silently corrupt
   the heap. — *Fix:* ban PMFs from DI bags; use `std::function<>` or a
   small virtual interface. Loses zero runtime perf (PMF through a bag is
   not hotter than `std::function` when both go through a cache-cold
   pointer), gains type-system robustness.

5. **[maint]** Orchestrators don't actually encapsulate — they defer to
   `FrameStateProvider(renderer)` to mutate state. `render_data_orchestrator.cpp`
   constructs `FrameStateProvider` in 5 places; `render_device_orchestrator.cpp`
   in 5 places. The DI bag of raw state pointers (`debug_config`,
   `performance_settings`, …) is half-redundant with the `FrameStateProvider`
   view, which reaches back through `renderer`. — *Why:* splits the state
   graph into two parallel access paths, one of which (`state_view`) is
   null-safe, the other (direct pointer) is not. Readers have to mentally
   reconcile which one is in use at each call site. — *Fix:* pick one.
   Either the orchestrator has typed state pointers (and no renderer
   back-reference) OR it reaches state exclusively through a state view
   interface. Mixing them doubles the cognitive load for nothing.

6. **[perf/maint]** `render_device_orchestrator.cpp:243-286` —
   `get_texture_format` walks up to 6 candidate devices, calling
   `acquire_submission_device_for` (which takes the GaussianSplatManager
   submission lock) on each one, and stops at the first valid texture.
   For every texture format query. — *Why:* lock-per-candidate is absurd
   when most of the time `device_state.rd` or the texture's
   owner-tracked device is the answer on the first try. The lock
   contention will show up under tile-heavy scenes. — *Fix:* check
   ownership-tracked first without acquiring the submission lock; only
   fall through to the full cascade on miss.

7. **[maint]** `render_device_orchestrator.cpp:35-37` — `_ensure_local_device`
   is a no-op shim (`return p_candidate;`) called in 4 places. Dead
   abstraction. Either inline it or give it a real validation body
   (check `is_local_device()` before returning). Right now every caller
   pays a function-call conceptual cost for zero filtering.

8. **[perf]** `render_resource_orchestrator.cpp:120-391` —
   `create_gpu_resources_safe` is a 270-line method that initializes
   buffer manager, painterly pipeline, shaders, GPU sorter, test data
   buffers, tile renderer, and interactive state manager — in one function
   with no early-exit contract for partial failure. Tile renderer init
   runs even if the buffer manager failed (gated only by log flags at
   lines 227-236). Resource leaks on partial failure are likely:
   `local_test_data_state.position_buffer` is overwritten at line 350
   but if the prior `free_owned_resource` at line 348 failed silently
   (no return value check), the old RID is still tracked by
   `RenderDeviceManager`. — *Fix:* split per-subsystem init into
   separate named functions with `Error` returns and strict "don't
   proceed if prior step failed" gating.

9. **[maint]** `render_data_orchestrator.cpp:235-381` —
   `update_gpu_buffers_with_real_data` duplicates a 10-field "reset
   streaming state" block at three sites (lines 114-127, 258-270,
   278-293, 319-334). Any new streaming field will require 4 edits and
   one will be missed. — *Fix:* extract a
   `StreamingState::reset_transient()` member function.

10. **[perf]** `render_data_orchestrator.cpp:309` — `acquire_rendering_device()`
    is called through a `std::function<>` on every `update_gpu_buffers_with_real_data`
    invocation, when `device_orchestrator->acquire_rendering_device()`
    is a direct member call that the renderer already has. Same for
    `release_shared_dynamic_asset` and `invalidate_static_chunk_caches`
    — all three are `std::function`s whose bodies contain a single
    renderer method call. — *Why:* std::function adds a heap allocation
    (typically eliminated by SBO) + a virtual-dispatch-equivalent
    indirection; there is no polymorphism benefit here since the callback
    always wraps the same renderer. — *Fix:* delete the lambdas, call
    the renderer directly; or promote them to typed interface methods.

11. **[maint]** `render_resource_orchestrator.cpp:120-127` — method copies
    8 member state pointers into local references
    (`GaussianSplatRenderer::TestDataState &local_test_data_state = *test_data_state;`
    etc.) at the top. This is equivalent to just using `test_data_state->…`
    everywhere but adds 8 lines of aliasing noise. The `local_` prefix
    gives a false impression that these are locally-owned state. — *Fix:*
    delete the aliases.

12. **[crash]** `render_data_orchestrator.cpp:388` — `subsystem_state.gpu_culler->get_state()`
    called unconditionally in `set_static_chunks`. If `gpu_culler` is
    unref'd (e.g. mid-teardown), this dereferences a null `Ref<>`.
    The code path isn't reached by normal teardown today, but
    `set_static_chunks` is publicly reachable from
    `GaussianSplatRenderer::set_static_chunks` (line 498) which is
    callable by any caller. — *Fix:* `ERR_FAIL_COND_MSG(!culler.is_valid(), …)`.

## Cross-cutting patterns

- **DI bag as god-object.** Both `RenderResourceOrchestrator::Dependencies`
  and `RenderDataOrchestrator::Dependencies` use the same anti-pattern:
  a flat POD with one pointer per piece of state the method happens to
  touch. Growth is linear; every new renderer member becomes a new
  DI field. This is the classic "struct of fields extracted to placate
  the linter, not to model a boundary" refactor. (`render_resource_orchestrator.h:17-29`,
  `render_data_orchestrator.h:35-44`)
- **Back-pointer + std::function capture both.** Every orchestrator holds
  `renderer` as a raw pointer AND holds 1-3 std::function callbacks that
  capture `[this]` (the renderer). Two parallel reach-back channels for
  the same object. Pick one. (`gaussian_splat_renderer.cpp:889-903`,
  `gaussian_splat_renderer.cpp:993-995`)
- **State returned by reference with static fallback on null.** Applied
  across `get_device_state`, `get_pipeline_state`, `get_resource_state`,
  `get_scene_state`, `get_streaming_state` — every state accessor on
  `GaussianSplatRenderer`. Any future orchestrator splits will extend
  this. The `static` fallback is a hidden singleton that crosses
  instance boundaries. (`gaussian_splat_renderer.cpp:2411-2538`)
- **PMF runtime ports.** Two of three orchestrators use PMFs. One already
  produced a heap corruption bug in the wild. The pattern is correct
  only if every TU that transitively sees the `RuntimePorts` struct also
  sees the full `GaussianSplatRenderer` definition. A forward-declaration
  anywhere in the include graph re-arms the trap.
  (`render_resource_orchestrator.h:8-15`, `render_data_orchestrator.h:27-29`)
- **Log-enable gating by flag lookup at callsite.** Every orchestrator method
  computes `log_enabled = debug_config.enable_* || …` at the top and gates
  a dozen log statements on it. Boilerplate that should live in a single
  logging macro / helper. (`render_data_orchestrator.cpp:9-16, 71-74, 142-147, 241-244`)
- **Pass-through renderer members.** `gaussian_splat_renderer.cpp:467-546`,
  every `_track_resource_owner`, `_free_owned_resource`, `_ensure_rendering_device`,
  etc. on `GaussianSplatRenderer` is literally `return device_orchestrator->…`.
  Zero added behavior, 80+ lines of forwarding. This is the cost of the
  extraction that should have been amortized against a real reduction
  in the god-class — which hasn't happened.

## Recommended refactor moves

### P0 (architecture freeze permits bug fixes only — these qualify)

- **[1 day]** Delete the `static …Fallback;` pattern in
  `get_*_state()` accessors. Replace with `T *` return + null handling
  at callsites. This is one mechanical sed + fix of the dozen callers
  that dereference unchecked. `gaussian_splat_renderer.cpp:2411-2538`.
- **[0.5 day]** Add `static_assert(offsetof(GaussianSplatRenderer, subsystem_state) <
  offsetof(GaussianSplatRenderer, device_orchestrator))` or an equivalent
  ordering comment block flagging the dtor contract for
  `subsystem_state` / orchestrator raw aliases. Make the landmine
  visible. Alternatively a `// ORDER MATTERS:` banner above the
  declarations.
- **[0.5 day]** Ban PMF runtime ports. Replace `RuntimePorts` in both
  `RenderResourceOrchestrator` and `RenderDataOrchestrator` with a
  small `IResourceHost` / `IDataHost` abstract class that
  `GaussianSplatRenderer` implements. Kills the ODR trap class.

### P1 (structural, >1 day, worthwhile)

- **[2-3 days]** Collapse the `Dependencies` bag. Both the resource and
  data orchestrators should take `GaussianSplatRenderer &` + typed
  accessor interfaces. The 10-pointer bag is a code smell serving no
  DI purpose (the raw pointers all alias the same renderer's members
  anyway).
- **[2 days]** Stop `RenderResourceOrchestrator::create_gpu_resources_safe`
  being a 270-line method. Split into
  `initialize_buffer_manager()`, `initialize_painterly()`,
  `initialize_shaders()`, `initialize_tile_renderer()`,
  `initialize_interactive_state()` — each with `Error` return and
  early-exit on prior failure.
- **[1 day]** Extract `StreamingState::reset_transient()` and replace the
  4 copies in `render_data_orchestrator.cpp`.
- **[1 day]** Remove the `std::function` callbacks that wrap single
  renderer method calls; call the renderer directly or promote to
  the typed host interface.

### P2 (nice-to-have, architecture freeze)

- **[0.5 day]** Inline `_ensure_local_device` or give it a validation
  body.
- **[1 day]** Rework `RenderDeviceOrchestrator::get_texture_format` to
  skip the full candidate walk when the ownership-tracked device is
  valid (perf; avoids N lock acquisitions).
- **[0.5 day]** Delete the `local_*` pointer aliases at the top of
  `create_gpu_resources_safe`.

## Blind spots

- `RenderDeviceManager` and `GPUSortingPipeline` implementations are
  out-of-slice — their behavior on null args and shutdown ordering
  affects whether the raw-alias hazard in issue #3 is crash-class or
  merely correctness-class. I assumed they tolerate a live raw alias
  during teardown because no current orchestrator destructor touches
  them.
- `GaussianSplatRenderer::FrameStateProvider` is out-of-slice. I relied
  on its behavior being "read/write-through to renderer members, null-
  safe if renderer is null." If it silently mutates a static fallback,
  issues #2 and #5 compound.
- `gaussian_splat_renderer.h`/`.cpp` were sampled, not fully read. The
  exact set of `.ptr()` raw aliases stashed at construction time was
  inferred from the ctor site and may have additional stale pointers
  elsewhere.
- Thread model: `RenderFrameContextManager::FrameState::visible_splat_count`
  is atomic but `frame_counter`, `sort_time_ms`, `render_time_ms` are
  plain `uint32_t`/`float` without synchronization. Whether they are
  mutated off the render thread is not visible from this slice's files
  alone. If they are, that's a data race.
- No static analysis was run. MSVC's observed sizeof mismatch for PMF
  may reoccur on clang if a future header reorg forward-declares
  `GaussianSplatRenderer` in `render_resource_orchestrator.h`. A
  `sizeof(…) == …` unit test per TU would catch regressions.

---

Spot-checks performed: (1) verified the `Dependencies` struct line numbers
at `render_resource_orchestrator.h:17-29` (correct; 10 pointers +
RuntimePorts); (2) verified `static …Fallback;` pattern at
`gaussian_splat_renderer.cpp:2517, 2524, 2530, 2536`; (3) verified
dtor-order invariant by reading `gaussian_splat_renderer.h:577-591` and
the ctor wiring at `gaussian_splat_renderer.cpp:887-999`; (4) verified
the MSVC PMF ODR trap comment at `render_data_orchestrator.h:8-16` and
matched it to the MEMORY.md record `project_msvc_pmf_odr_trap.md`;
(5) verified `_teardown_resources` ordering at
`gaussian_splat_renderer.cpp:1170-1300` (subsystem_state.device_manager
unref at line 1299 occurs inside the destructor body, before unique_ptr
destructors run).

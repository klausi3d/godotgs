# Renderer Monolith — Deep Audit

Unit 08 — `modules/gaussian_splatting/renderer/gaussian_splat_renderer.{cpp,h}` plus `gaussian_splat_renderer_bindings.cpp` and `debug_overlay_methods.cpp`.

## Summary

**Grade: D+**

The monolith has been *started* — twelve orchestrators hang off the renderer, and a pipeline-stages indirection sits in front of cull/sort/raster. But the façade still weighs in at **2,893 LOC of .cpp + 1,681 LOC of .h**, and the header alone carries the class body for the facade, eight nested state types, `FrameStateProvider` (38 virtuals), `FrameDeps` (15 pointers + a validate() method), and the shadow-blit state. Extraction is visibly half-finished: the constructor (lines 799–1026) is 228 lines of wiring spaghetti, `render_scene_instance` (2,078–2,322) is a 245-line switchboard, a full shadow-map path (1,523–1,804) is inlined, and an entire `FrameStateProvider` with `static <TypeName> fallback;` per accessor (39 total) leaks mutable statics into a supposedly RefCounted renderer. No fatal corruption bugs in the slice, but one crash-adjacent issue (shader-source leak on failed compile path retry), several maintainability disasters, and several per-frame `vformat`/`String` allocations that fire on every render path decision.

The earlier audit's flagged ranges (:876-1014, :1580-2045, :2002-2184) are **confirmed** — those are exactly the constructor tail, the shadow-blit / shadow-render block, and the render_scene_instance switchboard. I have more findings and tighter ranges below.

## What this code does

`GaussianSplatRenderer` is the main façade the engine-side RendererSceneRenderRD talks to. For each frame:

1. `render_scene_instance(RenderDataRD*)` is the entry (cpp:2078). It resets per-frame debug state, drains a deletion queue, sanity-checks the device, reads camera state from the scene data, builds a `FrameBackendPlan` (resident vs streaming policy), then delegates to either `_try_render_resident_frame` (2071) → `render_instanced` / `_run_cull_sort_pipeline_frame`, or `streaming_orchestrator->render_streaming_frame`.
2. `render_sorted_splats` (2349) is the shared pipeline driver — builds `RenderFrameContext`, a `FrameStateProvider`, a `RenderFramePlan`, then hands off to `RenderPipelineStages::render_sorted_splats_with_context`.
3. Ancillary responsibilities live in the same file: shadow map directional rendering (1641–1804), shadow-blit shader compile/pipeline cache (1566–1639), world-submission contract flags, streaming route-policy cache, RID lifetime (`_teardown_resources` 1170–1301), instance buffer upload (2660–2724), dispatch-on-render-thread wrappers, dozens of trivial `get_foo()` forwarders to orchestrators, and the full `FrameStateProvider` view/mutator interface (551–786).
4. `bindings.cpp` is all ClassDB/ADD_PROPERTY/BIND_ENUM (279 LOC, pure data) and `debug_overlay_methods.cpp` is 8 macro-generated setter/getter pairs (23 LOC).

## File anatomy — `gaussian_splat_renderer.cpp` (2,893 LOC)

| Range | Section | Notes |
|---|---|---|
| 1–87 | Includes + macros | 40+ include lines; `kLogFrameDebug` ifdef guard |
| 89–391 | Anonymous namespace helpers | Settings helpers, `FrameLogSettingsRegistry` singleton, route-UID string formatters, `_projection_nearly_equal`. Mostly stateless; could move to a helper TU. |
| 393–513 | Data-source / frame-plan builders | Thin delegators to `RenderPipelineStages` (build_frame_plan, build_data_source_plan, apply_data_source_plan) — essentially trampolines. |
| 515–549 | Cull projection contract | `build_cull_projection`, `validate_cull_projection_contract`, tile-renderer invalidation helpers. |
| 551–786 | **`FrameStateProvider` implementations** | 39 accessors, each with `static SceneState fallback;` (or equivalent) pattern. Header-declared class, cpp-defined body. |
| 787–797 | Render-thread dispatch trampolines | Two-liners forwarding to `render_thread_dispatcher`. |
| **799–1026** | **Constructor (228 LOC)** | 11 `.instantiate()` calls, 12 `std::make_unique<Orchestrator>` with full `Dependencies{}` struct wiring, runtime-port PMFs, lambdas capturing `this`. The central wiring mess. |
| 1028–1142 | Sort contract surface | Sort external-buffer state, sort byte-vector resize (cpu allocates), `publish_sorted_indices` with a per-frame `resize` + manual index copy loop (1114–1136). |
| 1144–1321 | Destructor + `_teardown_resources` | Settings disconnect, render-thread dispatch, 158-line resource cleanup enumerating every state bucket's RIDs. |
| 1322–1382 | Route-policy cache + runtime fidelity | `_mark_streaming_route_policy_dirty`, `build_runtime_fidelity_policy` — the ProjectSettings dirty-flag cache that actually looks decent. |
| 1384–1426 | `build_frame_backend_plan` | Decides resident vs streaming, fills String reason fields (several `String(...)` heap allocs per frame). |
| 1428–1488 | `initialize()` / render-thread variant | Timing block; calls `_create_gpu_resources_safe` → `initialize_sorting` → `_update_gpu_buffers_with_real_data`. |
| 1490–1521 | Painterly material setter / depth-range stub | `update_depth_range` is a no-op (1516). |
| **1523–1804** | **SHADOW MAP SUBSYSTEM (281 LOC)** | `ShadowBlitState::clear`, `_ensure_shadow_output_compositor`, `_ensure_shadow_blit_resources` (shader compile + pipeline cache), `_blit_shadow_depth`, `render_directional_shadow_map`. Has no business in this file. |
| 1806–1916 | Painterly depth, viewport color target, debug state-sync | `_get_painterly_depth_texture`, `_get_viewport_color_target`, `_check_dual_state_sync` (DEV_ENABLED stub), `_set_manual_viewport_format`. |
| 1917–1965 | Frame-context prep trampolines | `_prepare_render_frame_context`, `_run_pipeline_entry`, `_run_cull_sort_pipeline_frame`, `_reset_legacy_streaming_data_path_state`. |
| **1981–2076** | **Resident frame dispatch** | `_try_render_resident_frame` (95 LOC) and `_render_resident_frame` wrapper. Branches on contract readiness, legacy fallback. |
| **2078–2322** | **`render_scene_instance` — the switchboard (245 LOC)** | Per-frame entry. Reads camera, builds backend plan, dispatches to resident or streaming path, handles diagnostics + debug state. ~70 LOC of that is `#if DEBUG_ENABLED` vformat spam. |
| 2324–2347 | `tick_streaming_only`, `render_gaussians` wrapper | Tiny delegators. |
| 2349–2407 | `render_sorted_splats`, `get_final_texture`, `has_rendered_content`, `get_aabb` | The canonical sort-splat driver. |
| 2409–2540 | **State-accessor forwarders** | 30+ boilerplate `get_X()` / `get_X() const` pairs, each with `static FooState fallback;` + `ERR_FAIL_NULL_V(orchestrator, fallback)`. |
| 2541–2622 | Residency hints + instance contract publish | `get_submission_residency_hint`, `should_prefer_resident_backend`, `_set_route_policy_diagnostics`, `_set_instance_backend_diagnostics`, `_publish_resident_instance_pipeline_contract`. |
| 2624–2724 | Instance pipeline buffer upload | `publish_instance_pipeline_contract`, `update_instance_buffer` (GPU buffer alloc/grow). |
| 2726–2740 | Debug config/state forwarders | Direct delegators to `debug_state_orchestrator`. |
| **2742–2893** | `#ifdef TESTS_ENABLED` block | 16 test_* methods, ~151 LOC. Only referenced by tests. |

## Strengths

- **Orchestrator wiring is *principled*** — each `*_orchestrator` receives a `Dependencies{}` struct + runtime-port PMF table rather than raw `this` access. That's the right shape; it just hasn't finished reducing the façade.
- **Route-policy cache (1329–1350)** avoids re-reading `ProjectSettings` every frame via a `settings_changed` signal + dirty flag. Clean pattern.
- **`FrameDeps::validate()` (h:377–395)** — 12-field preflight with `ERR_FAIL_NULL_V_MSG` on each. Actually useful runtime sanity check at `render_sorted_splats` entry (cpp:2372).
- **Atomic `teardown_resources_started`** (h:592) prevents double teardown from the dual ~dtor / render-thread-dispatch path. Correct use of `compare_exchange_strong` (cpp:1172).
- **`FrameBackendPlan`** structure (h:240–259) turns the resident-vs-streaming decision into a plain-data value passed down, instead of branching logic scattered across the file. That is a pattern worth mimicking elsewhere.
- **Shadow-blit pipeline cache + sampler owner tracking** (1544–1639, 1621–1636) handles cross-device sampler ownership correctly — frees the old sampler if device changes before creating a new one.

## Top issues

**[severity: maint]** `gaussian_splat_renderer.cpp`:799–1026 — Constructor is 228 lines of dependency wiring with 11 `.instantiate()` + 12 `std::make_unique<Orchestrator>` blocks, each re-reading `get_*_state()` accessors that in turn require the orchestrators to already be constructed. — Why it matters: ordering is fragile (e.g. line 992 uses `subsystem_state.gpu_culler` which is instantiated at 820, fine today but silently order-dependent); a new orchestrator cannot be added without editing this file. — Fix direction: extract `RendererWiring::wire_orchestrators(GaussianSplatRenderer&)` free function into `renderer_wiring.cpp`; constructor body becomes 5–10 lines.

**[severity: maint]** `gaussian_splat_renderer.cpp`:1523–1804 (281 LOC) — The entire directional-shadow-map path lives inline in the facade: `ShadowBlitState::clear`, shadow output compositor ensure, shader compile + pipeline setup, sampler creation, `_blit_shadow_depth`, `render_directional_shadow_map`. — Why it matters: shadow rendering is an orthogonal subsystem with its own RID lifetime (`shadow_blit_state`, `shadow_output_compositor`, `shadow_output_device_id` on h:699–701), its own error-logging cadence, and is called exactly once from RendererSceneRenderRD. It has no reason to be in the facade. — Fix direction: extract to `renderer/shadow_blit_pass.{h,cpp}` owning `ShadowBlitState` + the two `_ensure_*` helpers; facade keeps a `std::unique_ptr<ShadowBlitPass>`.

**[severity: maint]** `gaussian_splat_renderer.cpp`:2078–2322 — `render_scene_instance` is a 245-LOC switchboard with 6 `#if defined(DEBUG_ENABLED) || kLogFrameDebug` blocks, 7 distinct fallback reason strings, and 3 backend-diagnostic helper calls. — Why it matters: per-frame log code (2130–2193) duplicates camera values, projection matrices, and route state across 15 `vformat` calls even when `_should_log_frame` gates are cheap; the control flow (prefer_resident → try_resident → fallthrough → try_streaming → fallthrough → resident) is non-linear. — Fix direction: lift the decision into a `FrameRouter::dispatch(backend_plan, view_state, deps)` that returns a discriminated result; move `DEBUG_ENABLED` blobs behind a single `debug_dump_camera_state(log_frame)` helper.

**[severity: maint]** `gaussian_splat_renderer.cpp`:551–786 + 2409–2540 — `FrameStateProvider` has 39 virtuals (view + mut), every one with a `static <Type> fallback;` pattern (`grep` confirms 39 occurrences). The state accessors in the facade (e.g. `get_scene_state()` at 2409, `get_render_config()` at 2421, etc.) duplicate the same pattern with `ERR_FAIL_NULL_V(orchestrator, fallback)`. — Why it matters: (a) mutable global statics in a supposedly clean facade; (b) if two renderer instances are ever destroyed concurrently you can alias — yes, one is `ERR_FAIL` path, but the contract is now "when an orchestrator is null, we silently return a shared mutable default". That's a gift-wrapped future bug. (c) the double abstraction (FrameStateProvider view + facade forwarder) means any read is two vcall + one static-init. — Fix direction: make the orchestrators *own* the state directly (they already do), delete the facade forwarders, and have `FrameStateProvider` take orchestrator pointers not a `GaussianSplatRenderer *`; kill the fallbacks — `ERR_FAIL_V` should return an empty `Ref<>` or crash loudly instead.

**[severity: perf]** `gaussian_splat_renderer.cpp`:177–190, 341–344, 2135, 2203–2219 — Per-frame `vformat` + `String` concatenation happens *regardless* of whether logging is enabled in places. Lines 177–190 build 4+ `vformat` strings unconditionally inside `_apply_resident_rejection_to_backend_plan` (called every frame that resident path is rejected and streaming kicks in); line 1417–1421 constructs a `String("world_submission_owns_streaming_path")` every frame even though it's a static label. — Why it matters: `vformat` allocates; on a 120 fps headset you'd rather not burn ~20 small heap allocs on diagnostics strings that nobody reads 99.99% of the time. — Fix direction: gate with `if (GaussianSplatting::debug_trace_is_enabled())` or lazily compute when first requested; use `StringName` or `const char *` for the static labels.

**[severity: perf]** `gaussian_splat_renderer.cpp`:2043–2044 — `LocalVector<Transform3D> instance_transforms; instance_transforms.push_back(Transform3D());` inside `_try_render_resident_frame`. This runs every frame the resident contract is ready. — Why it matters: single-element heap alloc on every resident frame, passed to `render_instanced` which presumably iterates it. — Fix direction: `static constexpr Transform3D kIdentitySingleton[1] = { Transform3D() };` or pass a `Span<const Transform3D>` that can wrap a stack local without heap.

**[severity: perf]** `gaussian_splat_renderer.cpp`:1106–1141 — `publish_sorted_indices` resizes a `LocalVector<uint8_t>` (`sort_index_bytes`) to `available_splats * sizeof(uint32_t)` every sort, then copies manually in a loop with two writes per splat (one to `cull_state.culled_indices`, one to `final_indices`). — Why it matters: for 1M splats that's an 8 MB worth of writes on the CPU each sort call to mirror what's already sitting on the GPU, plus the `buffer_update` at 1133 re-uploads. Both CPU copies serve "just in case someone reads the CPU mirror". Hot path. — Fix direction: make the CPU mirror lazy — only materialize when a non-GPU consumer asks; or at least fuse the two writes by aliasing `cull_state.culled_indices` data into `sort_index_bytes`.

**[severity: crash]** `gaussian_splat_renderer.cpp`:1587–1608 — If `version_build_variant_stage_sources` returns fewer than 2 stages, we `return false` at 1595 without calling `version_free` on `shader_version` (we *do* call it at 1593, so ok)... but more importantly, if the shader *compile* fails at 1605, the `shader_source_initialized = true` flag at 1584 is still set and `shader_source` is still held. On retry (next frame) we skip initialize (line 1577), call `version_create` (1588), rebuild sources, `version_free` — and loop again. If the compiler is deterministic that means every shadow frame burns a shader compile attempt silently. — Why it matters: failed shadow-blit compile = silent per-frame cost + no user-visible error after the initial warning at 1606. If `version_create` later leaks under failure, the leak is perpetual. — Fix direction: set a `bool shader_compile_failed` flag once and short-circuit; or move shadow-blit shader compile to a one-shot init that ERR_FAIL_V's permanently.

**[severity: crash]** `gaussian_splat_renderer.cpp`:1266–1294 — Teardown frees `gaussian_shader_source` via `version_free` + `memdelete` in `_teardown_resources`, but this path is gated on `get_device_state().rd` being non-null at 1264. If the RenderingDevice was destroyed before renderer teardown (e.g. device lost, or during shutdown race), `gaussian_shader_source` leaks because the delete at 1291–1294 is *outside* the `if (get_device_state().rd)` but `version_free` at 1267 is *inside* and never runs. — Why it matters: memdelete on the shader source without prior `version_free` might double-free GPU state or crash in shader-RD dtor. — Fix direction: move `version_free` out of the device-check guard, or track per-shader-version RIDs in the RAII deletion queue that's already in `resource_state`.

**[severity: corruption]** `gaussian_splat_renderer.h`:437–472 + cpp:605–786 — `FrameStateProvider` uses `static <Type> fallback;` inside 18 member functions. These are function-local statics, so each call site yields a *different* static — but they're named identically and all types are the same aggregate (`SceneState`, `StreamingState`, etc.). The `renderer_view == nullptr` branch returns a reference to this mutable static. — Why it matters: the contract says "if the renderer pointer is null, you get a reference to a shared mutable object"; any caller that writes to it via `get_X_mut()` (cpp:695–773) silently corrupts that static, and the next caller that falls through the ERR_FAIL path sees the corruption. This is a latent heisenbug waiting for a teardown race. — Fix direction: make `renderer_mut == nullptr` an abort (`ERR_FAIL_COND_MSG` → return from caller); never return a writable reference to a static fallback.

**[severity: maint]** `gaussian_splat_renderer.cpp`:1705, 1798, 1666, 2155, 2210, 2235 — Six `static bool`/`static int`/`static uint64_t` counter variables inside methods used for "log once" or "log every 60/300 frames". — Why it matters: they make the functions non-reentrant across renderer instances and leak debug state forever; also mean two renderers can't both emit a first-time warning. — Fix direction: move to `DebugState` fields or to `FrameLogSettingsRegistry` which already exists at 243–303.

**[severity: maint]** `gaussian_splat_renderer.h`:296–298 — `RenderFrameContextManager frame_context_manager;` is a *value-typed member* of the facade holding both `FrameState` and `ViewState`. Meanwhile `FrameState` is exposed *through* `get_frame_state()` which routes through orchestrators. So the facade simultaneously *owns* the frame/view state and *forwards* access to it via an orchestrator that refers back to the facade's member. — Why it matters: state ownership is genuinely unclear; dev_asserts like `_check_dual_state_sync` (1903) exist specifically because the two views could drift. The stub at 1912–1914 even admits "(void)p_context;" — the guardrail is not actually checking anything. — Fix direction: make `RenderFrameContextManager` a `std::unique_ptr` behind `device_orchestrator` or a new `RenderFrameOrchestrator`; remove the dual-state pattern entirely.

**[severity: perf]** `gaussian_splat_renderer.cpp`:2206–2207, 2330 — `build_frame_backend_plan(get_streaming_state().current_streaming_system.is_valid())` is called both in `render_scene_instance` (2206) and `tick_streaming_only` (2330); within `build_frame_backend_plan` itself (1384–1426) we hit `build_runtime_fidelity_policy` which calls `_refresh_streaming_route_policy_cache` (via const_cast at 1356 — ugh) and then `GaussianSplatSceneDirector::get_singleton()->has_world_submission_for_renderer(this)` (1406). — Why it matters: every frame does a singleton hash lookup + policy cache check twice. The `tick_streaming_only` + `render_scene_instance` combo means both run in sequence on frames where both happen. — Fix direction: cache `FrameBackendPlan` per-frame keyed on `frame_counter`.

**[severity: maint]** `gaussian_splat_renderer.cpp`:1516–1521 — `update_depth_range` is a no-op stub. — Why it matters: this is a public method that the engine calls. If it's genuinely a no-op, document it and remove the `(void)p_near; (void)p_far;` — both fields are in `p_near`/`p_far`. If it's not, fix it. — Fix direction: delete the method if engine integration doesn't need it, or wire it through to the culling/sorting depth hints.

**[severity: maint]** `gaussian_splat_renderer.cpp`:1903–1915 — `_check_dual_state_sync` is `DEV_ENABLED`-gated, then does `(void)p_context;` and returns. It's a placeholder that never actually checks sync. — Why it matters: three callers (`render_scene_instance` at 2089, etc.) rely on it for guardrails but it's inert. Dead safety. — Fix direction: either implement the check (compare `frame_context_manager.frame_state` against each orchestrator-returned `get_frame_state()`) or remove the method + call sites.

**[severity: maint]** `gaussian_splat_renderer.h` weighs 1,681 LOC and carries 17 nested type aliases (`using SceneState = ...` etc. at 193–199), 11 `std::unique_ptr<Orchestrator>` members (579–590), `FrameStateProvider` definition (437–472), `RenderFrameContext` + `FrameDeps` + `validate()` (333–397), `WorldSubmissionContract` (261–276), plus `ShadowBlitState` alias (327) and about 250 inline method bodies. — Why it matters: every TU that touches the facade pulls in all of this; compile-time tax is real; the header is load-bearing for the whole module. — Fix direction: forward-declare nested types, move `FrameStateProvider` to its own header, push `WorldSubmissionContract` / `WorldSubmissionRuntimeStateSnapshot` into `render_types/`.

## Cross-cutting patterns

- **"Facade delegates to orchestrator that holds back-pointer to facade."** Orchestrators are constructed with `this` as the renderer (see `RenderSortingOrchestrator::Dependencies::renderer = this;` at 923); they then call back into facade getters that forward to another orchestrator. This is circular and means no orchestrator is actually independent of the facade. Every orchestrator could be mocked in isolation *only if* it stopped calling back through `renderer->get_X()`.
- **"Static fallback on null orchestrator"**: the 39 `static FooState fallback;` pattern described above is a code smell of "we extracted the state but kept the access path; now we need a sentinel when the extraction isn't ready yet". The right move is to guarantee orchestrators exist for the lifetime of the facade and delete the fallbacks.
- **Per-frame String heap traffic**: `String(...)` + `vformat(...)` appear in hot paths (frame plan, diagnostics, backend-plan). `String` is ref-counted but vformat allocates.
- **Dispatch-on-render-thread dual paths**: `initialize`, `teardown`, `set_gaussian_data`, `force_sort_for_view`, and `_set_max_splats` each have a `_foo_on_render_thread(uint64_t request_id)` shadow; they call `_dispatch_call_on_render_thread_blocking`. The pattern is correct but repeated 6× — could be a template helper.
- **`#if defined(DEBUG_ENABLED) || kLogFrameDebug`** appears ≥5× (e.g. cpp:2002, 2130, 2143, 2172, 2221). Same conditional, same `if (should_log_frame) GS_LOG_RENDERER_DEBUG(...)` body. Extract a `RenderDebugTrace` helper.
- **Atomic `visible_splat_count.store(..., release)` / `load(acquire)`** is used correctly (9+ callsites) but the fact that `PerformanceState::metrics.rendered_splat_count` is then assigned the atomic's load at cpp:1140 means two copies of the same value exist — redundancy, and gives a window where they disagree.

## Proposed split seams

| New file | Line range to extract | Responsibility | Net effect on monolith |
|---|---|---|---|
| `renderer/shadow_blit_pass.{h,cpp}` | 1523–1639, `_blit_shadow_depth` 1641–1701, `render_directional_shadow_map` 1703–1804 | Shadow shader compile, sampler/pipeline cache, blit pass, directional shadow-map render. Owns its own `ShadowBlitState` (move h:699–702). | **−282 LOC** |
| `renderer/renderer_wiring.{h,cpp}` | Constructor body 799–1026 (keep ctor signature in facade, body becomes `RendererWiring::build(this, device)`). | Instantiate all subsystems, wire `Dependencies`+runtime-port PMFs, register with diagnostics/perf monitors. | **−228 LOC** |
| `renderer/render_scene_router.{h,cpp}` | `_try_render_resident_frame` 1981–2069, `_render_resident_frame` 2071–2076, `render_scene_instance` 2078–2322, `_reset_legacy_streaming_data_path_state` 1965–1979, `_apply_resident_rejection_to_backend_plan` 177–190. | Resident vs streaming dispatch, backend-plan processing, per-frame log dump. Reads state via `FrameStateProvider`, mutates via backend-plan; facade retains only a one-line `router->dispatch(p_render_data)`. | **−360 LOC** (incl. helpers) |
| `renderer/frame_state_provider.{h,cpp}` | `FrameStateProvider` cpp body 551–786, class definition h:437–472 | Dedicated view/mutation façade. Delete the `static FooState fallback;` pattern — require non-null orchestrators at construction. | **−236 LOC from cpp, −36 from h** |
| `renderer/renderer_state_accessors.{h,cpp}` | Lines 2409–2540 (state getter forwarders) | Pure trampolines from facade → orchestrator. If accessor bodies disappear into orchestrator-exposed public getters, this file disappears entirely. | **−131 LOC** |
| `renderer/renderer_tests.{cpp}` (TESTS_ENABLED only) | 2742–2893 `#ifdef TESTS_ENABLED` block | Test-only helpers. Keep as friend-class or public helpers compiled only in test builds. | **−151 LOC** |
| `renderer/renderer_bindings_properties.{cpp}` (already split) | bindings.cpp is fine; `debug_overlay_methods.cpp` is 23 LOC and can fold back into the debug orchestrator. | — | — |

After these extractions the .cpp drops from 2,893 → ~1,500 LOC, with every remaining block genuinely belonging to "the render façade's public surface + lifetime + common utilities."

## Recommended refactor moves

**P0 — crash/correctness (≤1 day each):**
- Fix the `FrameStateProvider` null-renderer fallback — replace `static FooState fallback;` with `ERR_FAIL_V` (~2 hrs).
- Fix shadow-blit shader retry loop (1577–1608) — latch a compile-failed flag to prevent infinite retry (~1 hr).
- Fix `_teardown_resources` shader-source leak when device is already gone (1266–1294) — move `version_free` outside the device guard or route through deletion queue (~2 hrs).
- Move the six per-method `static bool warned_*` / counters into `DebugState` or `FrameLogSettingsRegistry` (~2 hrs).

**P1 — structural (2–5 days each):**
- Extract `renderer/shadow_blit_pass.{h,cpp}` (−282 LOC, self-contained subsystem) — **2 days**.
- Extract `renderer/render_scene_router.{h,cpp}` (−360 LOC; hottest function) — **3 days**, carefully preserving every backend-plan branch and diagnostic reason string.
- Extract `renderer/renderer_wiring.{h,cpp}` for the constructor body — **1 day**.
- Delete `_check_dual_state_sync` stub OR implement it — **1 day** to decide + act.
- Cache `FrameBackendPlan` per-frame to stop double-building in render + tick — **½ day**.

**P2 — hygiene (1–3 days each):**
- Migrate per-frame `vformat`/`String` diagnostics to `const char *` or StringName — **2 days**, broad but mechanical.
- Move `#ifdef TESTS_ENABLED` block (2742–2893) to a dedicated `renderer_tests.cpp` — **½ day**.
- Fold `debug_overlay_methods.cpp` into `render_debug_state_orchestrator.cpp` — **½ day**.
- Investigate removing `RenderFrameContextManager` as a facade-owned value-type; either give it to `device_orchestrator` or to a new `RenderFrameOrchestrator` — **3 days**, touches dual-state guardrails.
- Delete `update_depth_range` stub OR implement it properly — **½ day**.
- Slim down `gaussian_splat_renderer.h` by forward-declaring nested types — **2 days**.

## Blind spots

- I did not audit `RenderPipelineStages`, `RenderDeviceOrchestrator`, `RenderDataOrchestrator`, `RenderSortingOrchestrator`, `RenderOutputOrchestrator`, `RenderStreamingOrchestrator`, `RenderQualityOrchestrator`, `RenderDiagnosticsOrchestrator`, `RenderInstancingOrchestrator`, `RenderConfigOrchestrator`, `RenderResourceOrchestrator`, `RenderDebugStateOrchestrator`. Those are other audit units. My claim that the facade is "half-extracted" relies on the orchestrators actually *owning* their state; if a subsequent audit finds they're hollow wrappers then the facade is even worse than I'm grading it.
- `GPUBufferManager`, `GPUSorter`, `gpu_memory_stream`, `GaussianStreamingSystem`, `PainterlyRenderer`, `OutputCompositor`, `TileRasterizer`, `GPUCuller` are only skimmed — I did not verify their internal shader compile paths or resource lifetimes. Shadow-blit compile is in scope (inline here), their compiles are not.
- I only read the top 500 LOC of the 1,681-line header and did not enumerate every inline method body. There may be additional inline functions doing per-frame work I didn't flag.
- The `test_*` methods (2742–2893) were treated as out-of-scope behavior; if the tests themselves are the only clients of some facade methods, that narrows the public surface considerably and would make the P1 extractions safer.
- `render_instanced` (called at cpp:2045) lives elsewhere — I did not trace its cost model.
- The `painterly_pass_graph.cpp` / `.h` (in my slice dir but not flagged in the LOC list) — I noted the header at 1823 (`_get_painterly_depth_texture`) uses it but didn't audit it for itself. The `painterly/` subdir is out of slice.
- `bindings.cpp` ClassDB surface: I did not cross-check every bound method actually exists on the facade (if one was renamed during extraction, bindings.cpp would happily bind an invalid symbol at link time — but this is a compile error, not a runtime hazard).

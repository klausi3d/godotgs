# Nodes (Godot Scene Integration) — Deep Audit

> **Unit 20 — Nodes slice.** Paths audited (under `modules/gaussian_splatting/nodes/`):
> `gaussian_splat_node_3d.{h,cpp}` (~2,363 LOC + 857-line header, the whale);
> `gaussian_splat_world_3d.{h,cpp}` (~551 + 93);
> `gaussian_splat_container.{h,cpp}` (~249 + 78);
> `gaussian_splat_dynamic_instance_3d.{h,cpp}` (~320 + 72);
> `gaussian_splat_debug_hud.{h,cpp}` (~194 + 160);
> `gaussian_splat_node_helpers.{h,cpp}` (~1,508 + 115).
> All six files read end-to-end. Spot-checks cross-referenced against
> the preliminary cosmic-stearns report (§5 renderer, §11 LOD, #257 RID-leak
> memory note).

## Summary

**Grade: C.**

The design intent here is defensible and, in several places, genuinely good:
`Ref<>`/`RID` everywhere, no raw `new`/`delete`, `ERR_FAIL_COND_V_MSG` at data
ingress in `set_splat_data`, explicit re-wiring of signals on asset swap,
two-track replay bookkeeping for color grading
(`grading_pushed_for_current_data` + `grading_explicit_pending`) that reads
like it was earned the hard way. The property and signal bindings are
mechanically correct. But the *node* layer as shipped is the emotional
landfill of the module: a 2,363-line `.cpp` + 857-line header that friends
six helper classes into its private state
(`gaussian_splat_node_3d.h:118-123`, `:244-249`), allocates `RID`s into engine
storage *from the constructor* before the node is in a tree
(`:295` `_ensure_gaussian_base()`), and leaves at least two audit-level
lifetime bugs visible on first read. Container serialization deliberately
uses local-vs-global transforms inconsistently
(`gaussian_splat_container.cpp:207-209`) and silently degrades. Dynamic
instance registration is single-path and shares most of the Node3D bugs
without benefiting from its helpers. The debug HUD is fine — the smallest and
cleanest file in the slice.

Production posture: **not ship-blocking on its own**, but lifetime issues in
`_ensure_gaussian_base()` and the eager-free pattern in the destructor
interact with issue #257's RID-leak cleanup in ways that will regress the
fix. Fix three P0 items (constructor-time storage allocation, dtor
RenderingServer liveness, helper-friend god-object decomposition) to move
this to B−.

---

## What this code does

Four scene-facing node classes plus two support classes:

- **`GaussianSplatNode3D`** — primary `Node3D` subclass. Owns asset loading
  (`ply_file_path` / `splat_asset`), per-instance quality/LOD/streaming
  config, painterly settings, shadow/opacity/wind overrides, color grading
  with bake/restore, ~20 debug-overlay toggles, editor preview, drag-drop,
  and the full render-instance lifecycle (`RS::instance_*` calls, gaussian
  storage base RID). Routes a large fraction of its behaviour through six
  helper classes declared as friends
  (`gaussian_splat_node_3d.h:118-123`). Registers with the singleton
  `GaussianSplatSceneDirector` (instance pipeline) and the
  `GaussianSplatManager` (update scheduler).

- **`GaussianSplatWorld3D`** — world-space unified renderer node. Consumes a
  `GaussianSplatWorld` resource (merged data + static chunks) and submits it
  through the director as a *world submission* rather than an instance
  registration (`gaussian_splat_world_3d.cpp:357-403`). Designed as the
  "single-renderer-for-large-scene" path.

- **`GaussianSplatContainer`** — editor-time merge helper. Iterates children
  that are `GaussianSplatNode3D`, collects their assets and transforms,
  calls `gaussian_splat_merge_sources` (out-of-slice) to produce
  `merged_data` + `merged_chunks`, then pushes the result into either a
  world resource (`export_world_resource`,
  `gaussian_splat_container.cpp:146-158`), a `GaussianSplatWorld3D`, or a
  `GaussianSplatNode3D` via `apply_to_node()` / `apply_to_renderer()`
  (`:86-139`).

- **`GaussianSplatDynamicInstance3D`** — lightweight instance-registry-only
  node. Does not render by itself; it registers a
  `Ref<GaussianSplatAsset>` with `GaussianSplatSceneDirector::register_instance`
  so the shared instance pipeline can stream/draw it alongside
  `GaussianSplatNode3D` instances.

- **`GaussianSplatDebugHUD`** — tiny `Control` that pulls a pre-formatted
  HUD-lines array out of `renderer->get_render_stats()["performance_hud_lines"]`
  and draws it to the viewport; polls at `update_interval` seconds
  (`gaussian_splat_debug_hud.cpp:49-58`).

- **Six helper classes** (`GaussianSplatNodeAssetHelper`,
  `GaussianSplatNodeViewportHelper`, `GaussianSplatNodeDebugHelper`,
  `GaussianSplatNodeQualityHelper`, `GaussianSplatNodeVisibilityHelper`,
  `GaussianSplatNodeRendererHelper`) — value-member subsystems that hold a
  `GaussianSplatNode3D &owner` reference and touch 50+ of its private fields.
  The header comment at `gaussian_splat_node_3d.h:113-117` calls this
  "intentionally tightly-coupled decomposition"; I call it what it is below.

---

## Strengths

1. **Property-/method-binding hygiene is solid.** Every setter has a matching
   getter, every enum has `BIND_ENUM_CONSTANT` + `VARIANT_ENUM_CAST`, property
   hints use the right `PROPERTY_HINT_*` flavour
   (`gaussian_splat_node_3d.cpp:60-281`, especially `:65, :69, :106, :120,
   :146, :244`). `set_splat_data` has a full `DEFVAL(...)` list at `:82-92`.
2. **Legacy compatibility via `_set`/`_get`** for `painterly/color_variation`
   and `rendering/occlusion_culling` (`gaussian_splat_node_3d.cpp:506-545`)
   is exactly the right pattern: keep old serialized scenes loading, hide
   the property from the editor.
3. **Signal wiring on asset swap is reconnected correctly.** `set_splat_asset`
   disconnects from the old asset before reassigning and connects to the new
   (`gaussian_splat_node_3d.cpp:577-587`). Mirrored in
   `set_color_grading` (`:2237-2244`) and in the dynamic instance
   (`gaussian_splat_dynamic_instance_3d.cpp:175-184`).
4. **Manual splat data path has real input validation.**
   `_validate_splat_data_inputs` enforces position-length equality for every
   optional array via `ERR_FAIL_COND_V_MSG`
   (`gaussian_splat_node_3d.cpp:648-703`). This is the best-looking trust
   boundary in the entire module slice — the I/O layer audit (§3) should
   steal this pattern.
5. **Viewport observer disconnect is thorough.**
   `disconnect_viewport_observers` nulls every cached pointer, clears every
   connection, and resets every state variable
   (`gaussian_splat_node_helpers.cpp:493-523`). Even handles the
   `ViewportTexture` `"changed"` signal. Uses `CONNECT_REFERENCE_COUNTED` so
   multiple binds don't multiply (`:482`, `:487`, `:543`).
6. **Color grading replay is carefully reasoned.** The 40-line comment at
   `gaussian_splat_node_3d.cpp:1594-1633` spells out *why* the two flags
   exist, what each prevents on a shared renderer (peer clobber during
   hot-reload, user-explicit-null being dropped on detached edit), and which
   tree transitions must *not* reset them. Rare to see this level of
   invariant documentation in a render path.
7. **Renderer-settings ownership is serialized through a global mutex.**
   `_claim_renderer_settings_owner` / `_release_renderer_settings_owner`
   (`gaussian_splat_node_helpers.cpp:33-78`) use `MutexLock` around a
   `HashMap<ObjectID, ObjectID>` and validate liveness of the previous
   owner via `ObjectDB::get_instance` before stealing it. This is the right
   pattern for a shared-renderer-multi-instance scene.
8. **Transform warning on non-identity world node**
   (`gaussian_splat_world_3d.cpp:351-354`) and on container→target apply
   (`gaussian_splat_container.cpp:111-113, :125-127`) — both cases where
   a non-identity transform produces a "double transform" visual bug. Warn
   once, keep rendering. Correct trade-off.

---

## Top issues

### Crash / corruption / lifetime

**[severity: crash]** `gaussian_splat_node_3d.cpp:295` — **Constructor calls
`_ensure_gaussian_base()` which calls
`RendererRD::GaussianSplatStorage::get_singleton()` and allocates an engine
RID before the node is in a tree.** The comment at `:1857-1875` shows
`gaussian_set_renderer(base, renderer)` being called while `renderer` is an
uninitialized `Ref<>`. If a script instantiates the node during editor
startup (before the rendering server singleton is up on some platforms) or
during headless tool setup, `get_singleton()` returns null and we silently
skip (tolerable), but if the node is freed *before ever entering the tree*,
`~GaussianSplatNode3D()` calls `_release_gaussian_base()` which touches the
storage singleton again — fine only because the guard repeats. **Worse**,
between construct and tree-entry, any call to `duplicate()` or variant-
assignment that triggers a copy does not re-run the constructor but *does*
share the non-owning `gaussian_base` RID, which is then double-freed when
either copy dies. **Why it matters:** mirrors the disease pattern of
issue #257 (RID leak exhausting element pool). **Fix direction:** move the
`_ensure_gaussian_base()` call out of the constructor and into
`NOTIFICATION_ENTER_WORLD`; destructor should be defensive only.

**[severity: crash]** `gaussian_splat_node_3d.cpp:308-317` — **Destructor
unconditionally calls `RS::get_singleton()->free(render_instance)` without
checking `RS::get_singleton()` is still alive.** On editor shutdown, the
`RenderingServer` can be torn down before node cleanup in certain orders
(tool-mode nodes, deferred frees). The same pattern in
`_notification_exit_tree` at `:385-388` checks validity but *also* skips the
null-check on `RS::get_singleton()`. Compare the cosmic-stearns
§7 "GPU Resource / RAII" finding: same root cause, different file. **Fix
direction:** `if (RenderingServer *rs = RS::get_singleton(); rs &&
render_instance.is_valid()) { rs->free(render_instance); }`, and guard
`_release_gaussian_base()` the same way.

**[severity: crash]** `gaussian_splat_node_3d.cpp:1867-1871` —
**`storage->gaussian_set_renderer(gaussian_base, renderer)` inside
`_ensure_gaussian_base()` is called with whatever `renderer` happens to
hold at the time.** That call runs from the constructor (`:295`) where
`renderer` is the default-constructed null `Ref<>`, from
`_update_bounds()` (`:1650`) which can fire on transform-change before any
renderer exists, and from `_update_render_instance()` (`:1852`) where the
renderer may be null for shared-renderer races. There is no
`ERR_FAIL_COND` on a null renderer, and the storage side has no contract
assertion visible from the header. **Why it matters:** if
`gaussian_set_renderer` is defined to accept null (unclear from the slice),
this is fine; if not, this is silent corruption that surfaces as "missing
splats until you jiggle the scene." **Fix direction:** explicitly test
`renderer.is_valid()` before the call, or document in the header that null
is the "clear" sentinel.

**[severity: corruption]** `gaussian_splat_node_3d.cpp:1983-1994` —
**`_set_instance_base()` mutates the `render_instance` base without
invalidating the previous base's `gaussian_set_renderer` linkage.** Sequence
to reproduce: `_ensure_gaussian_base()` allocates base A; data changes;
`_release_gaussian_base()` frees A; a subsequent `_ensure_gaussian_base()`
allocates base B; `_set_instance_base(B)` is called but the storage-side
mapping from the old base A's slot still references this node's renderer.
Because the release path at `:1884-1892` calls `gaussian_free(gaussian_base)`
*before* it nulls the local field (`:1894`), a re-entrant update between
those statements reads the just-freed base. **Fix direction:** null
`gaussian_base` *first*, then free. RAII pattern — move to a scoped helper.

**[severity: crash]** `gaussian_splat_dynamic_instance_3d.cpp:43-45` —
**Destructor calls `_unregister_instance()` which calls
`_unregister_instance_registry()` which calls
`GaussianSplatSceneDirector::get_singleton()`.** On shutdown, if the
director is torn down before the node, `get_singleton()` returns null and
we silently no-op — fine. But the instance registration at `:137-139`
happens unconditionally when `_can_register_instance()` succeeds, and
`registered` is a plain `bool` that can desync from the director's actual
state if the director rejects the registration (return value of
`register_instance` is discarded). **Fix direction:** thread the director's
acceptance back through the node's `registered` flag so the dtor doesn't
ask the director to unregister something that never registered.

### Lifecycle / notification

**[severity: maint]** `gaussian_splat_node_3d.cpp:319-348` —
**`_notification_enter_tree` does everything the node can possibly need to
do.** It wires the manager, loads the asset (`:333-335`), updates bounds
(`:336`), updates visibility (`:337`), finds the editor viewport (`:338`),
updates the render target cache (`:339`), ensures the renderer (`:344`),
updates the render instance (`:345`), registers with the shared renderer
(`:346`), updates the debug HUD (`:347`). `_notification_enter_world` at
`:350-358` then *does most of the same things again* — ensures renderer,
updates render instance, registers shared renderer, replays color grading,
applies debug settings. Godot fires `ENTER_WORLD` *after* `ENTER_TREE` for
3D nodes, so the work at `:344-347` is wasted and the renderer can flap
between tree-entry and world-entry states. **Fix direction:** put
world-dependent setup (`_ensure_renderer`, director registration) only in
`_notification_enter_world`; put tree-dependent setup (visibility tracking,
manager registration) only in `_notification_enter_tree`.

**[severity: perf]** `gaussian_splat_node_3d.cpp:360-393` —
**`_notification_exit_tree` releases GPU storage (`_release_gaussian_base()`
at `:383`) and the render instance RID (`:385-388`) but keeps `renderer`
valid and `renderer_data` valid.** The comment at `:372-380` explicitly
notes this is deliberate — to keep color-grading state coherent across
detach/re-attach — but the side effect is that *re-entering the tree* has
to re-allocate a new storage base, re-create a new render instance, and
re-run the whole 30-line enter_tree sequence. In editor drag-reparent this
fires per frame. Matches the cosmic-stearns §11.4 comment on
whole-asset-synchronous work stalling. **Fix direction:** either keep
storage allocated across detach (document the invariant) or audit which
downstream consumers actually require a fresh base RID on re-attach.

**[severity: maint]** `gaussian_splat_node_3d.cpp:417-456` —
**The `_notification` switch forwards to six helper methods
(`_notification_enter_tree`, `_notification_enter_world`, `_notification_exit_tree`,
`_notification_process`, `_notification_editor_post_save`) but there is no
handler for `NOTIFICATION_PREDELETE`.** On editor deletion, `PREDELETE`
fires before the destructor and is the correct place to release external
registrations (director, manager) so that inter-object destruction order
doesn't matter. Currently `EXIT_TREE` covers normal cases but not the
"freed while not in tree" case (which triggers when `splat_asset` mutation
queues a `memdelete` during editor rebuild). **Fix direction:** add
`NOTIFICATION_PREDELETE` that calls the shared-renderer unregister path and
disconnects asset signals defensively.

**[severity: maint]** `gaussian_splat_world_3d.cpp:82-135` —
**`GaussianSplatWorld3D::_notification` has no `ENTER_TREE` handler.**
All initialization happens in `NOTIFICATION_READY`, which only fires once
per tree lifetime. If the node is removed and re-added to the tree, the
director registration and renderer setup are not re-established until
`apply_world()` is manually called. `EXIT_TREE` does release (`:113-120`),
so the asymmetry is a silent bug: re-add leaves the world invisible. **Fix
direction:** split `NOTIFICATION_READY` into an `ENTER_TREE` path (register
+ apply) and a one-shot `READY` path (initial apply only). Mirror what
`GaussianSplatNode3D::_notification_enter_tree` already does.

### Binding / serialization

**[severity: maint]** `gaussian_splat_world_3d.cpp:34-80` — **`_bind_methods()`
has no `ADD_GROUP("Asset", "")` for the top-level `world` and
`auto_apply_on_ready` / `cast_shadow` properties.** `GaussianSplatNode3D`
at `:62, :95, :113, :143, :204` groups every property cluster; the world
node doesn't, so those top-three properties appear ungrouped above the
`Quality/` and `Rendering/` groups in the inspector. Minor polish, but
inconsistent with its sibling. Missing `ADD_SIGNAL` for world-load
success/failure; node3D has `asset_loaded` + `asset_loading_failed` at
`:278-280` but world has none — a failed world-resource load is
invisible to GDScript.

**[severity: maint]** `gaussian_splat_dynamic_instance_3d.cpp:14-37` —
**`_bind_methods()` missing `ADD_SIGNAL` and has no `ADD_GROUP`.** The
class *has* the concept of load failure (`_load_from_file` returns false
with a warn print at `:292`) but never emits a signal, so GDScript can't
react. Also, `ply_file_path` at `:17` is exposed with no `PROPERTY_HINT_FILE`
filter, even though the header/implementation only accepts `.ply`/`.spz`
and `set_ply_file_path` at `:162` emits a `WARN_DEPRECATED_MSG`. Drop the
property from the inspector (`PROPERTY_USAGE_NO_EDITOR`) if it's really
deprecated; otherwise give it the same file-filter hint as
`GaussianSplatNode3D::ply_file_path` at `:65`.

**[severity: maint]** `gaussian_splat_node_3d.cpp:298-302` —
**Constructor reads `GaussianSplatSettingsManager::load_debug_overlay_settings()`
and applies the result to four debug flags.** This happens unconditionally,
every construction, before any `_bind_methods` property restoration. When a
serialized scene assigns `debug/show_tile_grid = true`, the order is:
constructor overwrites with settings manager default (maybe false), then
Godot restores scene property (true). In this *particular* order it works,
but the fragile bit is that if the settings manager's defaults ever
diverge from the scene's persisted value, the scene wins — silently
contradicting the user's global debug settings. **Fix direction:** move the
settings-manager read to `NOTIFICATION_ENTER_TREE` *only* for fresh (non-
serialized) nodes, detectable via `get_tree()->get_edited_scene_root()` in
editor or by a sentinel value.

### Container / world correctness

**[severity: corruption]** `gaussian_splat_container.cpp:207-209` —
**The merger uses `get_global_transform()` when the node is in-tree and
`get_transform()` when it isn't, with a comment that rationalizes it as
"correct when all ancestors have identity transform."** Silent
2-regime behavior at a deterministic-output function is exactly the kind of
thing that passes a unit test and destroys a customer integration.
A container that is part of a scene where grandparent has a translation
will produce correctly-placed splats in-editor and wrongly-placed splats
when used offline from a tool script. **Fix direction:** require tree
attachment for `merge_children()` (add `ERR_FAIL_COND(!is_inside_tree())`),
or do the full parent-walk to compute accumulated transform; never
silently skip it.

**[severity: maint]** `gaussian_splat_container.cpp:160-169` —
**`clear_merged_data()` calls `merged_data->resize(0)` rather than
`merged_data.unref()`.** The `Ref<GaussianData>` is kept alive — pointing to
a zero-size data object — which defeats garbage collection and confuses the
`merged_data.is_null()` check in `apply_to_renderer` at `:89`. The function
will now return `ERR_UNAVAILABLE` correctly by the `get_count() == 0` branch,
but other callers reading `get_merged_data()` see a valid-but-empty ref and
may fail a `.is_null()` guard. **Fix direction:** `merged_data.unref()`.

**[severity: maint]** `gaussian_splat_container.cpp:71-84` —
**`merge_children()` silently returns when `sources.is_empty()` after
calling `clear_merged_data()` (from `_merge_children_internal()` at `:216-219`).** No warning to the user that their container has no
`GaussianSplatNode3D` children with assets. Editor UX: place container →
forget to add children → click "merge" → nothing happens → file bug. **Fix
direction:** `WARN_PRINT_ONCE` with the child count.

**[severity: maint]** `gaussian_splat_world_3d.cpp:34-80` (binding) +
`gaussian_splat_world_3d.cpp:168-174` (`set_world`) —
**`set_world()` doesn't disconnect any `changed` signal from the old world
resource, nor connect a signal on the new one.** Compare
`GaussianSplatNode3D::set_splat_asset` at `:577-593` which does both. If a
`GaussianSplatWorld` resource mutates (e.g. importer re-bakes in place),
the world3D node never reapplies it. **Fix direction:** mirror the
asset-signal pattern.

### Rendering / hot path

**[severity: perf]** `gaussian_splat_node_3d.cpp:1417-1456` —
**`process_gaussian_render()` calls `_is_renderer_shared_with_other_content`
every frame.** That function (`:40-57`) does two director hash-map lookups
on every tick. In a 50-node scene that's 100 director queries per frame
just to decide whether to flap `shared_renderer_multi_instance_state`.
Cache the result; invalidate on director-population change via an event, or
recompute on a throttled schedule. Matches the cosmic-stearns §5.4 pattern
of per-frame allocation/lookups in the renderer hot path.

**[severity: perf]** `gaussian_splat_node_3d.cpp:1167-1172` —
**`get_statistics()` iterates `render_stats.keys()` and copies every entry
into `stats`.** This is called on every inspector refresh (via
`NOTIFY_PROPERTY_LIST_CHANGED` in `_finalize_update_splats`). The key
iteration is `O(n)` on Godot's Dictionary and produces a Variant-typed
Array allocation each time. **Fix direction:** accept the renderer's dict
as-is and merge at the point of consumption, not at every stats fetch.

**[severity: perf]** `gaussian_splat_node_3d.cpp:1188-1195` —
**`update_splats()` uses `static int update_call_count`** to gate debug
log verbosity, which works fine functionally but means the count is shared
across every `GaussianSplatNode3D` in the process. Editor with five nodes
fills the 10-call debug window in the first two frames and then stays
quiet — log output is misleading. Move to `uint32_t update_call_count`
member.

**[severity: maint]** `gaussian_splat_node_3d.cpp:1013-1020` —
**`set_use_frustum_culling()` calls `_apply_renderer_settings()` which
eventually reaches
`renderer_helper.apply_renderer_settings()` (`gaussian_splat_node_helpers.cpp:1390-1465`),
itself ~75 lines of pushing 16 distinct knobs to the renderer.** Same for
every tiny setter (`set_opacity`, `set_max_splat_count`, …). Changing
frustum culling forces re-push of streaming-config overrides, painterly
settings, LOD bias, color grading, debug flags — a full renderer reset.
**Fix direction:** per-setter fine-grained push, or a "dirty mask" bitset
coalesced before the frame.

### Friend / encapsulation

**[severity: maint]** `gaussian_splat_node_3d.h:118-123, :244-249` —
**Six friend classes touching 50+ private fields.** The in-header comment
at `:114-117` justifies this as "larger encapsulation surface than
friendship." That would be a real argument if the helpers were small and
stable. Looking at `apply_renderer_settings` in the helper
(`gaussian_splat_node_helpers.cpp:1390-1465`) it directly reads
`owner.max_splat_count`, `owner.lod_bias`, `owner.max_render_distance`,
`owner.use_frustum_culling`, `owner.asset_optimize_for_gpu`,
`owner.enable_painterly`, `owner.edge_threshold`, `owner.stroke_opacity`,
`owner.stroke_width`, `owner.temporal_blend`, `owner.opacity`,
`owner.color_grading`, `owner.streaming_config`, `owner.lod_config`,
`owner.painterly_seed`, and calls `owner.debug_helper.apply_renderer_debug_settings()`.
That's 15 fields of coupled read, in *one* helper method. The cost of this
design is that `gaussian_splat_node_3d.h` is 857 lines, every private field
change propagates to six other files, and new contributors cannot tell
which class "owns" what behaviour. **Fix direction:** replace helper-as-
friend with helper-takes-`EffectiveSettings`-struct. Build the struct in the
node, pass it to the helper, helper becomes pure.

---

## Cross-cutting patterns

### Good repeated patterns (keep)

1. **Disconnect-then-connect on signal rewiring.** `set_splat_asset`,
   `set_color_grading`, `set_gaussian_data` all do it
   (`gaussian_splat_node_3d.cpp:577-587`, `:2237-2244`;
   `gaussian_splat_dynamic_instance_3d.cpp:175-184`). Zero dangling
   connects in the slice.
2. **`ERR_FAIL_COND_V_MSG` at array-length trust boundaries.**
   `_validate_splat_data_inputs` (`gaussian_splat_node_3d.cpp:648-703`).
   The I/O layer should copy this pattern verbatim.
3. **`CONNECT_REFERENCE_COUNTED` on viewport observer connects** (`gaussian_splat_node_helpers.cpp:482, :487, :543`) prevents
   accidental multi-bind.
4. **Non-identity transform warn-once** (`gaussian_splat_world_3d.cpp:351-354`; `gaussian_splat_container.cpp:111-113`). Right
   failure mode: don't block, don't silent-fail, tell the user.

### Bad repeated patterns (stop)

1. **GPU-storage allocation in constructor** — see top issue #1. Repeats in
   `GaussianSplatNode3D::GaussianSplatNode3D()` only, but the pattern of
   "ensure-lazy from any setter" blurs the line.
2. **Unchecked `RS::get_singleton()`** in destructors and `_set_instance_base`
   (`gaussian_splat_node_3d.cpp:315, :386, :1988-1991`). Half the call
   sites check, half don't.
3. **Silent `log-and-continue` on downstream error.**
   `runtime_asset->populate_from_gaussian_data` at `:798-801` logs a warn
   and proceeds; `merge_sources` failure at `:222-225` clears data silently;
   `sources.is_empty()` in container returns with no feedback. All match
   cosmic-stearns cross-cutting pattern "silent logs on critical failure."
4. **`_is_renderer_shared_with_other_content` called per-setter** — in
   `set_edge_threshold` (`:911-913`), `set_stroke_opacity` (`:924-926`),
   `set_stroke_width` (`:936-938`), `set_temporal_blend` (`:953-955`),
   `set_painterly_seed` (`:966-968`). Each call is a director map lookup.
   Redundant in the hot-set-many-at-once path.
5. **Helper-class-as-thin-forwarder** — most `GaussianSplatNode3D` public
   methods are a one-line `_delegate_helper.method()` with no logic
   (`:1097-1131`). Either the helper is meaningful (and should own more) or
   it isn't (and should be inlined back). Current state is worst-of-both.
6. **Non-identity transform detected but only on apply.** Three sites warn
   on non-identity; the container merge path *in `_merge_children_internal`*
   silently degrades to local transform (§ container issue above). Pattern
   is inconsistent — pick one.

---

## Recommended refactor moves

### P0 — Correctness, do this week (~1-2 eng-days)

1. **Move `_ensure_gaussian_base()` out of the constructor** into
   `_notification_enter_world`. Add `DEV_ASSERT(is_inside_world())` at the
   top of the function. Guard destructor with null-check on
   `RS::get_singleton()`. Cost: half a day. Prevents another #257-class bug.
2. **Audit every `RS::get_singleton()->free(...)`** in the slice (5 sites,
   grep matches at `:315, :386, :1984, …`) and wrap each in a
   `RenderingServer *rs = RS::get_singleton(); if (rs && rid.is_valid()) rs->free(rid);`
   helper. Cost: half a day.
3. **Add `NOTIFICATION_PREDELETE`** to `GaussianSplatNode3D` and
   `GaussianSplatWorld3D`. Unregister from director, disconnect asset
   signals, release `renderer_settings_owner` map entry. Cost: half a day.

### P1 — Quality, within 2 weeks (~1-2 eng-weeks)

4. **Split `_notification_enter_tree` and `_notification_enter_world`** so
   work happens exactly once at the right lifecycle stage (top issue #6).
   Add `ENTER_TREE` handler to `GaussianSplatWorld3D`. Cost: 1-2 days.
5. **Fix container transform ambiguity** — require `is_inside_tree()` for
   `merge_children()`, or walk the full parent chain to accumulate
   transform. Cost: half a day + test scene.
6. **Add `asset_loaded`/`asset_loading_failed` signals** to
   `GaussianSplatWorld3D` and `GaussianSplatDynamicInstance3D`; unify
   property hints (file filters) between the two. Cost: half a day.
7. **Stop calling `_apply_renderer_settings` from every small setter** —
   introduce a dirty bitset + one `flush_renderer_settings()` called at
   frame end. Cost: 2-3 days. Also kills a chunk of the
   `_is_renderer_shared_with_other_content` per-setter spam.
8. **`clear_merged_data()` should `unref()`, not `resize(0)`.** Three-line
   fix but wants a test. Cost: an hour.

### P2 — Architecture, within 6 weeks (~2-3 eng-weeks)

9. **Reduce friend-coupling.** Convert each helper from friend-with-ref to
   takes-`EffectiveSettings`-struct. Drop `gaussian_splat_node_3d.h` from
   ~860 lines toward ~500; helper files shrink proportionally. Start with
   `GaussianSplatNodeRendererHelper` (worst offender). Cost: 1 week of
   focused refactor + regression-test run.
10. **Split `GaussianSplatNode3D` into `Asset` + `Render` nodes.** The node
    currently wears five hats: asset owner, quality knob owner, debug-hud
    owner, render-instance owner, director-instance owner. A
    `GaussianSplatAssetNode3D` (owns asset, emits data-ready signals) + a
    `GaussianSplatRenderNode3D` (consumes data-ready, owns render-instance)
    would cut the class by 40% and let the dynamic-instance node share the
    asset-owning half cleanly. Cost: 2 weeks, worth it.

---

## Blind spots

Out of slice but heavily referenced — treat my claims about these as
"depends-on" rather than "verified":

- **`GaussianSplatSceneDirector`** — instance/world submission semantics,
  `register_instance`/`unregister_instance`/`submit_world_submission`
  ownership. I assumed these are thread-safe against concurrent
  register/unregister because the node callers implicitly do. If the
  director itself is not thread-safe, the global-mutex dance in
  `_claim_renderer_settings_owner` is fighting at the wrong layer.
- **`RendererRD::GaussianSplatStorage`** — the `gaussian_allocate` /
  `gaussian_set_renderer(base, Ref<>)` contract. I flagged the null-renderer
  call as suspect but did not read the storage source.
- **`GaussianSplatRenderer`** — `set_color_grading`, `invalidate_cached_render`,
  `get_scene_state`, `get_render_stats["performance_hud_lines"]`. Cosmic-
  stearns §5 says this class is 2,893 lines. Owning nodes do not appear to
  cache pointers across frames, so at the scene-integration boundary the
  renderer lifetime risk is bounded — but I cannot rule out that the
  `Ref<GaussianSplatRenderer>` kept in `GaussianSplatNode3D::renderer`
  across tree detach/re-attach outlives the `World3D` that spawned it.
- **`GaussianSplatManager` / `GaussianSplatSettingsManager`** — the
  constructor-time read of debug settings (`gaussian_splat_node_3d.cpp:297-302`)
  is only ordered-safe if those managers are initialized before any node is
  constructed. `register_types.cpp` (cosmic-stearns §14) was flagged for
  unasserted singleton init order.
- **`gaussian_splat_merge_sources` / `GaussianSplatMergeResult`** —
  container merge correctness under rotated/scaled child transforms is
  out-of-slice; I only verified that the local-vs-global conditional exists
  in the caller.
- **Test coverage of node lifecycle** — cosmic-stearns §13 counts 81 test
  files, 973 asserts. I did not read any tests for this slice to confirm
  whether the `NOTIFICATION_PREDELETE` gap, constructor-time RID alloc, or
  destructor `RS::get_singleton()` unchecked-free are currently exercised.
  Four pre-existing failures on base (memory note) may or may not touch
  these paths.

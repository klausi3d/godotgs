extends SceneTree

const ASSET_PATH := "res://tests/fixtures/test_splats.ply"
const SKIP_MARKER := "[RUNTIME_SKIP]"
const FAIL_MARKER := "[RUNTIME_FAIL]"
const METRICS_MARKER := "[RUNTIME_METRICS]"
const MAX_RENDERER_WAIT_FRAMES := 120

var failures: Array[String] = []
var metrics := {
	"frames": 0,
	"baseline_matched_count": 0,
	"baseline_position_active": false,
	"baseline_opacity_active": false,
	"outside_subtree_matched_count": 0,
	"world_scope_outside_matched_count": 0,
	"explicit_scope_matched_count": 0,
	"target_opacity_partial_active": false,
	"target_opacity_neutral_active": false,
}

var scene_root: Node3D
var runtime_camera: Camera3D
var effect_group: Node3D
var default_branch: Node3D
var scoped_branch: Node3D
var outside_group: Node3D
var effector: SphereEffector3D
var node_a: GaussianSplatNode3D
var node_b: GaussianSplatNode3D
var node_c: GaussianSplatNode3D
var node_d: GaussianSplatNode3D

func _init() -> void:
	call_deferred("_run")

func _is_headless_runtime() -> bool:
	return OS.has_feature("headless") or DisplayServer.get_name() == "headless"

func _record_check(condition: bool, label: String, context: Dictionary = {}) -> void:
	if condition:
		print("PASS ", label)
		return
	_record_failure(label, context)

func _record_failure(label: String, context: Dictionary = {}) -> void:
	var reason := label
	if not context.is_empty():
		reason = "%s | context=%s" % [label, str(context)]
	failures.append(reason)
	push_error("%s %s" % [FAIL_MARKER, reason])

func _emit_metrics(status: String, reason: String = "") -> void:
	var payload := metrics.duplicate(true)
	payload["status"] = status
	if reason != "":
		payload["reason"] = reason
	print("%s %s" % [METRICS_MARKER, JSON.stringify(payload)])

func _cleanup() -> void:
	if scene_root != null:
		scene_root.queue_free()
	scene_root = null
	runtime_camera = null
	effect_group = null
	default_branch = null
	scoped_branch = null
	outside_group = null
	effector = null
	node_a = null
	node_b = null
	node_c = null
	node_d = null

func _create_splat_node(asset: GaussianSplatAsset, node_name: String, node_position: Vector3) -> GaussianSplatNode3D:
	var node := GaussianSplatNode3D.new()
	node.name = node_name
	node.splat_asset = asset
	node.position = node_position
	node.set_effect_position_scale(1.0)
	node.set_effect_opacity_scale(1.0)
	return node

func _await_runtime_sync(frame_count: int = 2) -> void:
	for i in range(frame_count):
		await process_frame

func _record_node_state(node: GaussianSplatNode3D, label: String, expected_count: int, expect_position_active: bool, expect_opacity_active: bool) -> void:
	var matched_count := int(node.get_last_matched_scene_effector_count())
	var position_active := bool(node.is_scene_effector_position_active())
	var opacity_active := bool(node.is_scene_effector_opacity_active())
	var context := {
		"matched_count": matched_count,
		"position_active": position_active,
		"opacity_active": opacity_active,
	}
	_record_check(matched_count == expected_count, "%s matched count" % label, context)
	_record_check(position_active == expect_position_active, "%s position activity" % label, context)
	_record_check(opacity_active == expect_opacity_active, "%s opacity activity" % label, context)

func _record_statistics_surface(node: GaussianSplatNode3D, label: String) -> void:
	var stats := node.get_statistics()
	_record_check(stats.has("matched_scene_effectors"), "%s statistics include matched_scene_effectors" % label, {"stats": stats})
	_record_check(stats.has("scene_effector_position_active"), "%s statistics include scene_effector_position_active" % label, {"stats": stats})
	_record_check(stats.has("scene_effector_opacity_active"), "%s statistics include scene_effector_opacity_active" % label, {"stats": stats})
	if stats.has("matched_scene_effectors"):
		_record_check(
			int(stats["matched_scene_effectors"]) == int(node.get_last_matched_scene_effector_count()),
			"%s statistics matched_scene_effectors mirrors runtime query" % label,
			{"stats": stats}
		)
	if stats.has("scene_effector_position_active"):
		_record_check(
			bool(stats["scene_effector_position_active"]) == bool(node.is_scene_effector_position_active()),
			"%s statistics position flag mirrors runtime query" % label,
			{"stats": stats}
		)
	if stats.has("scene_effector_opacity_active"):
		_record_check(
			bool(stats["scene_effector_opacity_active"]) == bool(node.is_scene_effector_opacity_active()),
			"%s statistics opacity flag mirrors runtime query" % label,
			{"stats": stats}
		)

func _setup_scene() -> bool:
	scene_root = Node3D.new()
	scene_root.name = "SceneEffectorRuntimeControls"
	root.add_child(scene_root)

	runtime_camera = Camera3D.new()
	runtime_camera.name = "RuntimeCamera"
	runtime_camera.position = Vector3(0.0, 2.0, 8.0)
	runtime_camera.look_at(Vector3.ZERO, Vector3.UP)
	runtime_camera.make_current()
	scene_root.add_child(runtime_camera)

	effect_group = Node3D.new()
	effect_group.name = "EffectGroup"
	scene_root.add_child(effect_group)

	default_branch = Node3D.new()
	default_branch.name = "DefaultBranch"
	effect_group.add_child(default_branch)

	scoped_branch = Node3D.new()
	scoped_branch.name = "ScopedBranch"
	effect_group.add_child(scoped_branch)

	outside_group = Node3D.new()
	outside_group.name = "OutsideGroup"
	scene_root.add_child(outside_group)

	var asset := GaussianSplatAsset.new()
	var load_err := asset.load_from_file(ASSET_PATH)
	if load_err != OK:
		_record_failure("Failed to load fixture asset", {"path": ASSET_PATH, "err": load_err})
		return false

	effector = SphereEffector3D.new()
	effector.name = "RuntimeEffector"
	effector.enabled = true
	effector.radius = 12.0
	effector.strength = 1.0
	effector.falloff = 2.0
	effector.frequency = 2.0
	effector.affect_position = true
	effector.affect_opacity = true
	effector.opacity_strength = 0.8
	effector.target_opacity = 0.35
	effect_group.add_child(effector)

	node_a = _create_splat_node(asset, "NodeA", Vector3(-1.5, 0.0, 0.0))
	default_branch.add_child(node_a)

	node_b = _create_splat_node(asset, "NodeB", Vector3(1.5, 0.0, 0.0))
	node_b.set_scene_effector_layer_mask(2)
	default_branch.add_child(node_b)

	node_c = _create_splat_node(asset, "NodeC", Vector3(0.0, 0.0, 0.0))
	outside_group.add_child(node_c)

	node_d = _create_splat_node(asset, "NodeD", Vector3(0.0, 0.0, -2.0))
	node_d.set_scene_effector_scope_root(NodePath(".."))
	scoped_branch.add_child(node_d)

	return true

func _wait_for_renderer() -> GaussianSplatRenderer:
	for i in range(MAX_RENDERER_WAIT_FRAMES):
		await process_frame
		metrics["frames"] = i + 1
		if node_a != null:
			var renderer: GaussianSplatRenderer = node_a.get_renderer()
			if renderer != null:
				return renderer
	return null

func _run() -> void:
	if _is_headless_runtime():
		var skip_reason := "Scene effector runtime controls require non-headless execution."
		_emit_metrics("skipped", skip_reason)
		print("%s %s" % [SKIP_MARKER, skip_reason])
		quit(0)
		return

	if not _setup_scene():
		_emit_metrics("failed", "scene_setup_failed")
		_cleanup()
		quit(1)
		return

	var renderer := await _wait_for_renderer()
	if renderer == null:
		var skip_reason := "Renderer unavailable (local RenderingDevice required)."
		_emit_metrics("skipped", skip_reason)
		print("%s %s" % [SKIP_MARKER, skip_reason])
		_cleanup()
		quit(0)
		return

	await _await_runtime_sync()
	metrics["baseline_matched_count"] = node_a.get_last_matched_scene_effector_count()
	metrics["baseline_position_active"] = node_a.is_scene_effector_position_active()
	metrics["baseline_opacity_active"] = node_a.is_scene_effector_opacity_active()
	metrics["outside_subtree_matched_count"] = node_c.get_last_matched_scene_effector_count()

	_record_node_state(node_a, "Node A baseline subtree match", 1, true, true)
	_record_node_state(node_b, "Node B layer-mask exclusion", 0, false, false)
	_record_node_state(node_c, "Node C default subtree exclusion", 0, false, false)
	_record_node_state(node_d, "Node D node-side scope narrowing", 0, false, false)
	_record_statistics_surface(node_a, "Node A")

	node_b.set_scene_effector_layer_mask(1)
	await _await_runtime_sync()
	_record_node_state(node_b, "Node B layer-mask opt-in", 1, true, true)

	node_b.set_scene_effector_layer_mask(2)
	await _await_runtime_sync()
	_record_node_state(node_b, "Node B layer-mask opt-out", 0, false, false)

	effector.scope_mode = SphereEffector3D.SCOPE_WORLD
	await _await_runtime_sync()
	metrics["world_scope_outside_matched_count"] = node_c.get_last_matched_scene_effector_count()
	_record_node_state(node_a, "Node A world scope", 1, true, true)
	_record_node_state(node_c, "Node C world scope inclusion", 1, true, true)
	_record_node_state(node_d, "Node D world scope still filtered by node scope", 0, false, false)

	effector.scope_mode = SphereEffector3D.SCOPE_EXPLICIT_ROOT
	effector.scope_root = NodePath("../ScopedBranch")
	await _await_runtime_sync()
	metrics["explicit_scope_matched_count"] = node_d.get_last_matched_scene_effector_count()
	_record_node_state(node_a, "Node A explicit-root exclusion", 0, false, false)
	_record_node_state(node_c, "Node C explicit-root exclusion", 0, false, false)
	_record_node_state(node_d, "Node D explicit-root inclusion", 1, true, true)
	_record_statistics_surface(node_d, "Node D")

	effector.scope_mode = SphereEffector3D.SCOPE_SUBTREE
	effector.scope_root = NodePath("")
	await _await_runtime_sync()
	_record_node_state(node_a, "Node A subtree scope restored", 1, true, true)
	_record_node_state(node_c, "Node C subtree scope restored", 0, false, false)
	_record_node_state(node_d, "Node D subtree scope restored", 0, false, false)

	effector.enabled = false
	await _await_runtime_sync()
	_record_node_state(node_a, "Disabled effector", 0, false, false)
	_record_node_state(node_c, "Disabled effector outside subtree", 0, false, false)

	effector.enabled = true
	effector.target_opacity = 0.6
	await _await_runtime_sync()
	metrics["target_opacity_partial_active"] = node_a.is_scene_effector_opacity_active()
	_record_node_state(node_a, "Re-enabled effector with partial target opacity", 1, true, true)

	effector.affect_opacity = false
	await _await_runtime_sync()
	_record_node_state(node_a, "Opacity channel disabled at runtime", 1, true, false)

	effector.affect_opacity = true
	effector.target_opacity = 1.0
	await _await_runtime_sync()
	metrics["target_opacity_neutral_active"] = node_a.is_scene_effector_opacity_active()
	_record_node_state(node_a, "Neutral target opacity remains matched but inert", 1, true, false)

	effector.target_opacity = 0.6
	await _await_runtime_sync()
	_record_node_state(node_a, "Partial target opacity restores active opacity channel", 1, true, true)

	node_a.set_scene_effectors_enabled(false)
	await _await_runtime_sync()
	_record_node_state(node_a, "Node A scene-effector toggle off", 0, false, false)

	node_a.set_scene_effectors_enabled(true)
	await _await_runtime_sync()
	_record_node_state(node_a, "Node A scene-effector toggle on", 1, true, true)

	node_a.set_effect_position_scale(0.0)
	node_a.set_effect_opacity_scale(0.0)
	await _await_runtime_sync()
	_record_node_state(node_a, "Node A zero response scales", 1, false, false)

	node_a.set_effect_position_scale(1.0)
	node_a.set_effect_opacity_scale(1.0)
	node_a.set_opacity(0.0)
	await _await_runtime_sync()
	_record_node_state(node_a, "Node A base opacity zero", 1, true, false)
	_record_statistics_surface(node_a, "Node A after base opacity zero")

	_emit_metrics("passed" if failures.is_empty() else "failed")
	_cleanup()
	quit(0 if failures.is_empty() else 1)

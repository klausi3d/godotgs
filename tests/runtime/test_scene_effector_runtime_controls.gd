extends SceneTree

const ASSET_PATH := "res://tests/fixtures/test_splats.ply"
const SKIP_MARKER := "[RUNTIME_SKIP]"
const FAIL_MARKER := "[RUNTIME_FAIL]"
const METRICS_MARKER := "[RUNTIME_METRICS]"
const MAX_RENDERER_WAIT_FRAMES := 120

var failures: Array[String] = []
var metrics := {
	"frames": 0,
	"matched_count": 0,
	"position_active": false,
	"opacity_active": false,
}

var scene_root: Node3D
var runtime_camera: Camera3D
var effect_group: Node3D
var effector: SphereEffector3D
var node_a: GaussianSplatNode3D
var node_b: GaussianSplatNode3D

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
	effector = null
	node_a = null
	node_b = null

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

	node_a = GaussianSplatNode3D.new()
	node_a.name = "NodeA"
	node_a.splat_asset = asset
	node_a.position = Vector3(-1.5, 0.0, 0.0)
	node_a.set_effect_position_scale(1.0)
	node_a.set_effect_opacity_scale(1.0)
	effect_group.add_child(node_a)

	node_b = GaussianSplatNode3D.new()
	node_b.name = "NodeB"
	node_b.splat_asset = asset
	node_b.position = Vector3(1.5, 0.0, 0.0)
	node_b.set_scene_effector_layer_mask(2)
	effect_group.add_child(node_b)

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

	await process_frame
	metrics["matched_count"] = node_a.get_last_matched_scene_effector_count()
	metrics["position_active"] = node_a.is_scene_effector_position_active()
	metrics["opacity_active"] = node_a.is_scene_effector_opacity_active()

	_record_check(node_a.get_last_matched_scene_effector_count() == 1, "Node A matches one scene effector")
	_record_check(node_a.is_scene_effector_position_active(), "Node A reports active position deformation")
	_record_check(node_a.is_scene_effector_opacity_active(), "Node A reports active opacity modulation")
	_record_check(node_b.get_last_matched_scene_effector_count() == 0, "Node B is excluded by layer mask")
	_record_check(not node_b.is_scene_effector_position_active(), "Node B position effect stays inactive")
	_record_check(not node_b.is_scene_effector_opacity_active(), "Node B opacity effect stays inactive")

	effector.enabled = false
	await process_frame
	_record_check(node_a.get_last_matched_scene_effector_count() == 0, "Disabling effector clears runtime matches")
	_record_check(not node_a.is_scene_effector_position_active(), "Disabled effector clears position activity")
	_record_check(not node_a.is_scene_effector_opacity_active(), "Disabled effector clears opacity activity")

	effector.enabled = true
	effector.target_opacity = 0.6
	await process_frame
	_record_check(node_a.get_last_matched_scene_effector_count() == 1, "Re-enabling effector restores runtime matches")

	node_a.set_effect_position_scale(0.0)
	node_a.set_effect_opacity_scale(0.0)
	await process_frame
	_record_check(node_a.get_last_matched_scene_effector_count() == 1, "Match count remains visible when response scales are zeroed")
	_record_check(not node_a.is_scene_effector_position_active(), "Zero position scale disables active position contribution")
	_record_check(not node_a.is_scene_effector_opacity_active(), "Zero opacity scale disables active opacity contribution")

	node_a.set_effect_opacity_scale(1.0)
	node_a.set_opacity(0.0)
	await process_frame
	_record_check(not node_a.is_scene_effector_opacity_active(), "Base opacity 0 keeps opacity contribution inactive")

	_emit_metrics("passed" if failures.is_empty() else "failed")
	_cleanup()
	quit(0 if failures.is_empty() else 1)

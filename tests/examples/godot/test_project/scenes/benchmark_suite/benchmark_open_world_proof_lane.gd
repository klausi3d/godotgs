extends "res://scenes/benchmark_suite/benchmark_suite_lane.gd"

const BenchmarkOpenWorldStageContract = preload("res://scripts/benchmark_open_world_stage_contract.gd")
const WORLD_CONTRACT_KIND := "gaussian_world_contract"

@onready var streaming_world: GaussianSplatWorld3D = $StreamingWorld

var _world_stage_contract_path := ""
var _world_stage_result: Dictionary = {}


func _resolved_asset_path() -> String:
	var scene_id := BenchmarkSceneContract.scene_id_from_path(get_tree().current_scene.scene_file_path)
	return BenchmarkSceneContract.resolve_world_contract_path(scene_id, lane_id, asset_override_path)


func _build_instances(config: Dictionary) -> void:
	for child in instance_root.get_children():
		child.queue_free()
	_instance_nodes.clear()
	_primary_renderer_owner = null
	_max_node_visible_splats = 0
	_max_total_visible_splats = 0
	_world_stage_result.clear()
	_world_stage_contract_path = _resolved_asset_path()

	if streaming_world == null:
		push_error("[BENCH-WORLD] Missing StreamingWorld node.")
		return
	if _world_stage_contract_path.is_empty():
		push_error("[BENCH-WORLD] Missing open-world stage contract path.")
		return

	_world_stage_result = BenchmarkOpenWorldStageContract.build_world_from_stage_manifest(_world_stage_contract_path, self)
	var world = _world_stage_result.get("world")
	if world == null:
		push_error("[BENCH-WORLD] Failed to build world from %s (%s)" % [
			_world_stage_contract_path,
			str(_world_stage_result.get("error", "unknown_error")),
		])
		return

	streaming_world.set("quality/max_splat_count", int(config.get("max_splats", 50000)))
	streaming_world.set("quality/lod_bias", float(config.get("lod_bias", 1.1)))
	streaming_world.set("quality/max_render_distance", float(config.get("lod_max_distance", 400.0)))
	streaming_world.set_world(world)
	streaming_world.apply_world()
	_primary_renderer_owner = streaming_world


func _resolve_focus_point() -> Vector3:
	if streaming_world != null and streaming_world.has_method("get_world"):
		var world = streaming_world.get_world()
		if world != null and world.has_method("get_bounds"):
			var bounds: AABB = world.get_bounds()
			return bounds.position + bounds.size * 0.5
	return super._resolve_focus_point()


func _sample_metrics(delta: float) -> void:
	super._sample_metrics(delta)
	var renderer = _get_primary_renderer()
	if renderer != null and renderer.has_method("get_visible_splat_count"):
		var visible := int(renderer.get_visible_splat_count())
		if visible > _max_total_visible_splats:
			_max_total_visible_splats = visible
		if visible > _max_node_visible_splats:
			_max_node_visible_splats = visible


func _apply_renderer_overrides_from_config() -> void:
	super._apply_renderer_overrides_from_config()
	if streaming_world == null or not streaming_world.has_method("get_renderer"):
		return

	var has_distance_cull_enabled := _lane_config.has("distance_cull_enabled")
	var has_distance_cull_start := _lane_config.has("distance_cull_start")
	var has_distance_cull_max_rate := _lane_config.has("distance_cull_max_rate")
	var has_tiny_radius := _lane_config.has("tiny_splat_screen_radius")
	var has_overflow_autotune := _lane_config.has("overflow_autotune_enabled")
	if not has_distance_cull_enabled and not has_distance_cull_start and not has_distance_cull_max_rate and not has_tiny_radius and not has_overflow_autotune:
		return

	var renderer = streaming_world.get_renderer()
	if renderer == null:
		return
	if has_distance_cull_enabled and renderer.has_method("set_distance_cull_enabled"):
		renderer.set_distance_cull_enabled(bool(_lane_config.get("distance_cull_enabled")))
	if has_distance_cull_start and renderer.has_method("set_distance_cull_start"):
		renderer.set_distance_cull_start(float(_lane_config.get("distance_cull_start")))
	if has_distance_cull_max_rate and renderer.has_method("set_distance_cull_max_rate"):
		renderer.set_distance_cull_max_rate(float(_lane_config.get("distance_cull_max_rate")))
	if has_tiny_radius and renderer.has_method("set_tiny_splat_screen_radius"):
		renderer.set_tiny_splat_screen_radius(float(_lane_config.get("tiny_splat_screen_radius")))
	if has_overflow_autotune and renderer.has_method("set_overflow_autotune_enabled"):
		renderer.set_overflow_autotune_enabled(bool(_lane_config.get("overflow_autotune_enabled")))


func _build_report() -> Dictionary:
	var report: Dictionary = super._build_report()
	report["asset_path"] = _world_stage_contract_path
	report["asset_resource_kind"] = WORLD_CONTRACT_KIND
	report["open_world_stage_result"] = _world_stage_result
	report["open_world_stage_contract_path"] = _world_stage_contract_path
	return report

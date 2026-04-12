extends "res://scenes/benchmark_suite/benchmark_suite_lane.gd"

const BenchmarkOpenWorldStageContract = preload("res://scripts/benchmark_open_world_stage_contract.gd")
const WORLD_CONTRACT_KIND := "gaussian_world_contract"

@onready var streaming_world: GaussianSplatWorld3D = $StreamingWorld

var _world_stage_contract_path := ""
var _world_stage_result: Dictionary = {}
var _data_aabb := AABB()
var _diag8_count := 0
var _raster_counters_enabled := false
var _capture_armed := false


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

	var world: GaussianSplatWorld = null
	if _world_stage_contract_path.ends_with(".gsplatworld"):
		# Pre-built staged world — load directly, no runtime synthesis.
		world = load(_world_stage_contract_path) as GaussianSplatWorld
		if world == null:
			push_error("[BENCH-WORLD] Failed to load staged world from %s" % _world_stage_contract_path)
			return
		_world_stage_result = {
			"world": world,
			"staged_world_path": _world_stage_contract_path,
			"generated_chunk_count": world.get_chunk_count() if world.has_method("get_chunk_count") else 0,
		}
	else:
		# Runtime synthesis from stage manifest (bootstrap path).
		_world_stage_result = BenchmarkOpenWorldStageContract.build_world_from_stage_manifest(_world_stage_contract_path, self)
		world = _world_stage_result.get("world")
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


	# Cache gaussian data AABB for camera sweep derivation.
	if world.has_method("get_gaussian_data"):
		var gdata = world.get_gaussian_data()
		if gdata != null and gdata.has_method("get_aabb"):
			_data_aabb = gdata.get_aabb()
			# Override corridor sweep length from actual data extent.
			var z_extent := absf(_data_aabb.size.z)
			if z_extent > 1.0:
				config["corridor_length"] = z_extent * 0.45
				config["corridor_speed"] = z_extent * 0.045


func _resolve_focus_point() -> Vector3:
	# Prefer the gaussian data AABB (actual splat positions) over the world
	# bounds (which may reflect chunk metadata in a different coordinate space).
	if streaming_world != null and streaming_world.has_method("get_world"):
		var world = streaming_world.get_world()
		if world != null:
			# Try gaussian_data AABB first — this reflects actual splat positions.
			if world.has_method("get_gaussian_data"):
				var gdata = world.get_gaussian_data()
				if gdata != null and gdata.has_method("get_aabb"):
					var data_bounds: AABB = gdata.get_aabb()
					if data_bounds.size.length() > 0.01:
						return data_bounds.position + data_bounds.size * 0.5
			# Fall back to world bounds.
			if world.has_method("get_bounds"):
				var bounds: AABB = world.get_bounds()
				return bounds.position + bounds.size * 0.5
	return super._resolve_focus_point()


func _sample_metrics(delta: float) -> void:
	super._sample_metrics(delta)
	var renderer = _get_primary_renderer()
	# Enable binning counters so COUNT/EMIT divergence stats flow into the
	# OverflowStats buffer. count_pass_* fields are unconditional in the shader;
	# raster_splats_* require GS_COLLECT_RASTER_STATS (enabled by
	# set_debug_binning_counters_enabled). We skip debug_show_performance_hud
	# here because its GDScript-side HUD rendering has %u format errors that
	# crash _process.
	if not _raster_counters_enabled and renderer != null:
		if renderer.has_method("set_debug_binning_counters_enabled"):
			renderer.set_debug_binning_counters_enabled(true)
		_raster_counters_enabled = true
	# Arm a sync CPU capture of tile_counts + tile_ranges once the scene has
	# ramped enough to be interesting (frame counters are approximate).
	if not _capture_armed and renderer != null and _max_total_visible_splats > 300000:
		if renderer.has_method("debug_arm_one_shot_count_range_capture"):
			renderer.debug_arm_one_shot_count_range_capture()
			_capture_armed = true
			print("[DIAG-BENCH] Armed one-shot CPU capture at visible=%d" % _max_total_visible_splats)
	if renderer != null and renderer.has_method("get_visible_splat_count"):
		var visible := int(renderer.get_visible_splat_count())
		if visible > _max_total_visible_splats:
			_max_total_visible_splats = visible
		if visible > _max_node_visible_splats:
			_max_node_visible_splats = visible
	# DIAG-8: log renderer visible splat count for streaming world proof validation.
	if renderer != null:
		var vis_count := int(renderer.get_visible_splat_count()) if renderer.has_method("get_visible_splat_count") else -1
		var stats: Dictionary = renderer.get_render_stats() if renderer.has_method("get_render_stats") else {}
		var vis_after_cull: int = int(stats.get("visible_after_culling", -1))
		var uploaded: int = int(stats.get("uploaded_splat_count", -1))
		var atlas_pub := -1
		if Performance.has_custom_monitor("gaussian_splatting/streaming_atlas_published_chunks"):
			atlas_pub = int(Performance.get_custom_monitor("gaussian_splatting/streaming_atlas_published_chunks"))
		if _diag8_count < 30 or _diag8_count % 60 == 0:
			print("[DIAG-BENCH-VIS] vis_splat_count=%d vis_after_cull=%d uploaded=%d atlas_pub_chunks=%d max_total=%d max_node=%d" % [
				vis_count, vis_after_cull, uploaded, atlas_pub, _max_total_visible_splats, _max_node_visible_splats])
			# Tile-overflow telemetry for the overlap checkpoint.
			var overlap_records: int = int(stats.get("overlap_records", 0))
			var overlap_budget: int = int(stats.get("overlap_record_budget_effective", 0))
			var total_tiles: int = int(stats.get("total_tiles", 0))
			var tiles_with_overflow: int = int(stats.get("tiles_with_overflow", 0))
			var max_splats_in_tile: int = int(stats.get("max_splats_in_tile", 0))
			var avg_splats_per_tile: float = float(stats.get("average_splats_per_tile", 0.0))
			var overflow_ratio: float = float(stats.get("overflow_ratio", 0.0))
			var overflow_clamped: int = int(stats.get("overflow_splats_clamped", 0))
			var overflow_aggregated: int = int(stats.get("overflow_splats_aggregated", 0))
			var raster_iter: int = int(stats.get("raster_splats_iterated", 0))
			var raster_contrib: int = int(stats.get("raster_splats_contributed", 0))
			var raster_time_ms: float = float(stats.get("gpu_tile_raster_time_ms", 0.0))
			var binning_time_ms: float = float(stats.get("gpu_tile_binning_time_ms", 0.0))
			print("[DIAG-BENCH-OVERFLOW] overlaps=%d/%d tiles=%d overflow_tiles=%d max_pt=%d avg_pt=%.1f overflow_ratio=%.3f clamped=%d aggregated=%d iter=%d contrib=%d binning_ms=%.2f raster_ms=%.2f" % [
				overlap_records, overlap_budget, total_tiles,
				tiles_with_overflow, max_splats_in_tile, avg_splats_per_tile, overflow_ratio,
				overflow_clamped, overflow_aggregated, raster_iter, raster_contrib,
				binning_time_ms, raster_time_ms])
			# COUNT/EMIT divergence diagnostics. If emit_entered > count_entered,
			# early-return predicates diverge (conic/distance/eigen/bbox/lens_fade).
			# overflow_splats_aggregated is the EMIT-side attempted insert count,
			# so it should match count_pass_accepts under perfect parity. The
			# separate clamped counter records how many of those attempts were
			# dropped by the per-tile or global overlap budget.
			var count_entered: int = int(stats.get("count_pass_entered", 0))
			var emit_entered: int = int(stats.get("emit_pass_entered", 0))
			var count_accepts: int = int(stats.get("count_pass_accepts", 0))
			var emit_attempts: int = overflow_aggregated
			var emit_estimated_writes: int = maxi(emit_attempts - overflow_clamped, 0)
			var pass_delta: int = emit_entered - count_entered
			var accept_delta: int = emit_attempts - count_accepts
			print("[DIAG-BENCH-DIVERGENCE] count_entered=%d emit_entered=%d (delta=%d) count_accepts=%d emit_attempts=%d (delta=%d) emit_clamped=%d emit_est_writes=%d" % [
				count_entered, emit_entered, pass_delta, count_accepts, emit_attempts, accept_delta, overflow_clamped, emit_estimated_writes])
			# Per-tile triad for a single probe tile. COUNT's count vs the range.y
			# that prefix scan produced vs EMIT's attempts/accepts. Decision rule:
			# * count > range.y => prefix/range construction bug
			# * count == range.y && emit_attempts > range.y => shader-local/aliasing
			var probe_tile: int = int(stats.get("probe_tile_idx", 0))
			var probe_count: int = int(stats.get("probe_count_accepts", 0))
			var probe_emit_attempts: int = int(stats.get("probe_emit_attempts", 0))
			var probe_emit_accepts: int = int(stats.get("probe_emit_accepts", 0))
			var probe_range_y: int = int(stats.get("probe_range_y_seen", 0))
			var probe_range_x: int = int(stats.get("probe_range_x_seen", 0))
			print("[DIAG-BENCH-PROBE-TILE] idx=%d count=%d range_y=%d range_x=%d emit_attempts=%d emit_accepts=%d" % [
				probe_tile, probe_count, probe_range_y, probe_range_x, probe_emit_attempts, probe_emit_accepts])
			# First tile that clamped this frame. Shader stores tile_idx+1, so 0
			# means "no clamp captured" and any non-zero value decodes to tile_idx.
			var clamp_tile_plus1: int = int(stats.get("first_clamp_tile_idx", 0))
			var clamp_range_y: int = int(stats.get("first_clamp_range_y", 0))
			var clamp_local_offset: int = int(stats.get("first_clamp_local_offset", 0))
			var clamp_valid: bool = clamp_tile_plus1 > 0
			var clamp_tile: int = clamp_tile_plus1 - 1 if clamp_valid else -1
			print("[DIAG-BENCH-FIRST-CLAMP] tile_idx=%d range_y=%d local_offset=%d valid=%s" % [
				clamp_tile, clamp_range_y, clamp_local_offset, "Y" if clamp_valid else "N"])
			# Use the most recent clamping tile as the probe tile for next frame.
			# This aims the probe at a GUARANTEED divergent tile (count>=range.y).
			if clamp_valid and renderer.has_method("set_debug_probe_tile_idx"):
				renderer.set_debug_probe_tile_idx(clamp_tile)
		_diag8_count += 1


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

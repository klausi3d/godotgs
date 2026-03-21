#include "render_diagnostics_orchestrator.h"

#include "../core/gs_project_settings.h"
#include "render_debug_state_orchestrator.h"

#include "core/config/project_settings.h"
#include "core/error/error_macros.h"
#include "core/math/math_defs.h"
#include "core/math/math_funcs.h"
#include "core/math/vector2i.h"
#include "core/os/os.h"
#include "core/string/ustring.h"
#include "core/variant/variant.h"
#include "rendering_diagnostics.h"
#include "sorting_config.h"
#include "gpu_sorter.h"
#include "../interfaces/debug_overlay_system.h"
#include "../interfaces/gpu_sorting_pipeline.h"
#include "../interfaces/rasterizer_interfaces.h"
#include "../logger/gs_logger.h"

namespace {
static String _stage_status_to_string(GaussianSplatRenderer::StageResult::StageStatus p_status) {
	switch (p_status) {
		case GaussianSplatRenderer::StageResult::StageStatus::SUCCESS:
			return "success";
		case GaussianSplatRenderer::StageResult::StageStatus::SKIPPED:
			return "skipped";
		case GaussianSplatRenderer::StageResult::StageStatus::FALLBACK:
			return "fallback";
		case GaussianSplatRenderer::StageResult::StageStatus::FAILED:
			return "failed";
	}
	return "unknown";
}

static String _normalize_route_uid_for_stats(const String &p_route_uid) {
	if (p_route_uid.is_empty()) {
		return RenderRouteUID::COMMON_UNKNOWN_ROUTE;
	}
	return p_route_uid;
}

static String _normalize_sort_route_uid_for_stats(const String &p_sort_route_uid) {
	if (p_sort_route_uid.is_empty()) {
		return RenderRouteUID::COMMON_UNKNOWN_SORT_ROUTE;
	}
	return p_sort_route_uid;
}

struct ProductionMetricsConfig {
	bool validate_metrics = true;
	uint32_t summary_interval_frames = 600;
	uint32_t summary_history_size = 60;
	bool perf_gate_enabled = false;
	uint32_t perf_gate_splat_threshold = 100000;
	float perf_gate_budget_ms = 16.0f;
};

// Project settings helpers provided by gs_project_settings.h (gs::settings namespace).
static uint32_t _get_uint_setting(ProjectSettings *p_settings, const StringName &p_name, uint32_t p_fallback) {
	return gs::settings::get_uint(p_settings, p_name, p_fallback);
}

static float _get_float_setting(ProjectSettings *p_settings, const StringName &p_name, float p_fallback) {
	return gs::settings::get_float(p_settings, p_name, p_fallback);
}

static bool _get_bool_setting(ProjectSettings *p_settings, const StringName &p_name, bool p_fallback) {
	return gs::settings::get_bool(p_settings, p_name, p_fallback);
}

static ProductionMetricsConfig _load_production_metrics_config() {
	ProductionMetricsConfig config;
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (!settings) {
		return config;
	}
	config.validate_metrics = _get_bool_setting(settings,
			"rendering/gaussian_splatting/diagnostics/validate_production_metrics",
			config.validate_metrics);
	config.summary_interval_frames = _get_uint_setting(settings,
			"rendering/gaussian_splatting/diagnostics/summary_interval_frames",
			config.summary_interval_frames);
	config.summary_history_size = _get_uint_setting(settings,
			"rendering/gaussian_splatting/diagnostics/summary_history_size",
			config.summary_history_size);
	config.perf_gate_enabled = _get_bool_setting(settings,
			"rendering/gaussian_splatting/diagnostics/perf_gate_enabled",
			config.perf_gate_enabled);
	config.perf_gate_splat_threshold = _get_uint_setting(settings,
			"rendering/gaussian_splatting/diagnostics/perf_gate_splat_threshold",
			config.perf_gate_splat_threshold);
	config.perf_gate_budget_ms = _get_float_setting(settings,
			"rendering/gaussian_splatting/diagnostics/perf_gate_budget_ms",
			config.perf_gate_budget_ms);
	if (config.summary_interval_frames == 0) {
		config.summary_interval_frames = 1;
	}
	if (config.summary_history_size == 0) {
		config.summary_history_size = 1;
	}
	if (config.perf_gate_budget_ms < 0.0f) {
		config.perf_gate_budget_ms = 0.0f;
	}
	return config;
}

static Array _production_metrics_contract() {
	Array keys;
	keys.push_back("frame");
	keys.push_back("visible_splats");
	keys.push_back("total_splats");
	keys.push_back("cull_ms");
	keys.push_back("sort_ms");
	keys.push_back("raster_ms");
	keys.push_back("composite_ms");
	keys.push_back("stage_total_ms");
	keys.push_back("render_ms");
	keys.push_back("frame_time_ms");
	keys.push_back("gpu_frame_ms");
	keys.push_back("gpu_binning_ms");
	keys.push_back("gpu_prefix_ms");
	keys.push_back("gpu_raster_ms");
	keys.push_back("gpu_resolve_ms");
	keys.push_back("gpu_timing_frame_serial");
	keys.push_back("gpu_timing_frames_behind");
	keys.push_back("gpu_pass_breakdown_available");
	keys.push_back("data_source");
	keys.push_back("data_source_error");
	keys.push_back("raster_path");
	keys.push_back("render_mode");
	keys.push_back("stage_metrics_valid");
	keys.push_back("stage_cull_status");
	keys.push_back("stage_sort_status");
	keys.push_back("stage_raster_status");
	keys.push_back("stage_composite_status");
	keys.push_back("route_uid");
	keys.push_back("sort_route_uid");
	return keys;
}

static bool _is_finite(float p_value) {
	return !(Math::is_nan(p_value) || Math::is_inf(p_value));
}

static int64_t _dict_get_i64(const Dictionary &p_dict, const char *p_key, int64_t p_default = 0) {
	const StringName key(p_key);
	if (!p_dict.has(key)) {
		return p_default;
	}
	return static_cast<int64_t>(p_dict[key]);
}

static float _dict_get_f32(const Dictionary &p_dict, const char *p_key, float p_default = 0.0f) {
	const StringName key(p_key);
	if (!p_dict.has(key)) {
		return p_default;
	}
	return static_cast<float>(p_dict[key]);
}

static bool _dict_get_bool(const Dictionary &p_dict, const char *p_key, bool p_default = false) {
	const StringName key(p_key);
	if (!p_dict.has(key)) {
		return p_default;
	}
	return static_cast<bool>(p_dict[key]);
}

static Dictionary _build_production_metrics_snapshot(GaussianSplatRenderer &p_renderer,
		const GaussianSplatRenderer::StageMetrics &p_stage_metrics, bool p_stage_valid, float p_frame_time_ms) {
	Dictionary metrics;
	const auto &frame_state = p_renderer.get_frame_state();
	const auto &data_source_info = p_renderer.get_performance_state().data_source_info;
	const auto &debug_state = p_renderer.get_debug_state();
	uint32_t visible_splats = frame_state.visible_splat_count.load(std::memory_order_acquire);
	const auto &scene_state = p_renderer.get_scene_state();
	uint32_t total_splats = scene_state.gaussian_data.is_valid()
			? scene_state.gaussian_data->get_count()
			: 0;
	float cull_ms = p_stage_valid ? p_stage_metrics.cull.cull_time_ms : 0.0f;
	float sort_ms = p_stage_valid ? p_stage_metrics.sort.sort_time_ms : 0.0f;
	float raster_ms = p_stage_valid ? p_stage_metrics.raster.render_time_ms : 0.0f;
	float composite_ms = p_stage_valid ? p_stage_metrics.composite_time_ms : 0.0f;
	float stage_total_ms = cull_ms + sort_ms + raster_ms;

	// Read GPU timing directly from TileRenderer/rasterizer when available.
	float gpu_frame_ms = 0.0f;
	float gpu_binning_ms = 0.0f;
	float gpu_prefix_ms = 0.0f;
	float gpu_raster_ms = 0.0f;
	float gpu_resolve_ms = 0.0f;
	int64_t gpu_timing_frame_serial = 0;
	int64_t gpu_timing_frames_behind = 0;
	const bool has_tile_renderer = p_renderer.get_tile_renderer_state().renderer.is_valid();
	if (has_tile_renderer) {
		const TileRenderer *tr = p_renderer.get_tile_renderer_state().renderer.ptr();
		gpu_frame_ms = tr->get_last_gpu_frame_time_ms();
		gpu_binning_ms = tr->get_last_gpu_binning_time_ms();
		gpu_prefix_ms = tr->get_last_gpu_prefix_time_ms();
		gpu_raster_ms = tr->get_last_gpu_raster_time_ms();
		gpu_resolve_ms = tr->get_last_gpu_resolve_time_ms();
		RasterPerformance rp = p_renderer.get_subsystem_state().rasterizer->get_performance();
		gpu_timing_frame_serial = static_cast<int64_t>(rp.timing_frame_serial);
		gpu_timing_frames_behind = static_cast<int64_t>(rp.timing_frames_behind);
	}

	metrics["frame"] = static_cast<int64_t>(frame_state.frame_counter);
	metrics["visible_splats"] = static_cast<int64_t>(visible_splats);
	metrics["total_splats"] = static_cast<int64_t>(total_splats);
	metrics["cull_ms"] = cull_ms;
	metrics["sort_ms"] = sort_ms;
	metrics["raster_ms"] = raster_ms;
	metrics["composite_ms"] = composite_ms;
	metrics["stage_total_ms"] = stage_total_ms;
	metrics["render_ms"] = raster_ms;
	metrics["frame_time_ms"] = p_frame_time_ms;
	metrics["gpu_frame_ms"] = gpu_frame_ms;
	metrics["gpu_binning_ms"] = gpu_binning_ms;
	metrics["gpu_prefix_ms"] = gpu_prefix_ms;
	metrics["gpu_raster_ms"] = gpu_raster_ms;
	metrics["gpu_resolve_ms"] = gpu_resolve_ms;
	metrics["gpu_timing_frame_serial"] = gpu_timing_frame_serial;
	metrics["gpu_timing_frames_behind"] = gpu_timing_frames_behind;
	metrics["gpu_pass_breakdown_available"] = gpu_binning_ms > 0.0f ||
			gpu_prefix_ms > 0.0f ||
			gpu_raster_ms > 0.0f ||
			gpu_resolve_ms > 0.0f;
	metrics["data_source"] = data_source_info.data_source;
	metrics["data_source_error"] = data_source_info.data_source_error;
	String raster_path = "unknown";
	if (p_stage_valid && !p_stage_metrics.raster.raster_path.is_empty()) {
		raster_path = p_stage_metrics.raster.raster_path;
	}
	metrics["raster_path"] = raster_path;
	const auto &render_config = p_renderer.get_render_config();
	metrics["render_mode"] = static_cast<int64_t>(render_config.render_mode);
	metrics["stage_metrics_valid"] = p_stage_valid;
	metrics["stage_cull_status"] = p_stage_valid ? _stage_status_to_string(p_stage_metrics.cull_result.status) : String("unknown");
	metrics["stage_sort_status"] = p_stage_valid ? _stage_status_to_string(p_stage_metrics.sort_result.status) : String("unknown");
	metrics["stage_raster_status"] = p_stage_valid ? _stage_status_to_string(p_stage_metrics.raster_result.status) : String("unknown");
	metrics["stage_composite_status"] = p_stage_valid ? _stage_status_to_string(p_stage_metrics.composite_result.status) : String("unknown");
	metrics["route_uid"] = debug_state.route_uid;
	metrics["sort_route_uid"] = debug_state.sort_route_uid;

	return metrics;
}

static void _append_telemetry_extras(GaussianSplatRenderer &p_renderer,
		const GaussianSplatRenderer::StageMetrics &p_stage_metrics, bool p_stage_valid,
		float p_frame_time_ms, Dictionary &r_metrics) {
	const auto &frame_state = p_renderer.get_frame_state();
	const auto &data_source_info = p_renderer.get_performance_state().data_source_info;
	const auto &sorting_state = p_renderer.get_sorting_state();
	const auto &debug_state = p_renderer.get_debug_state();
	const auto &subsystem_state = p_renderer.get_subsystem_state();
	r_metrics["frame_count"] = static_cast<int64_t>(frame_state.frame_counter);
	r_metrics["sorted_splats"] = static_cast<int64_t>(sorting_state.sorted_splat_count);
	r_metrics["frame_time_ms"] = p_frame_time_ms;
	// Frame timing aggregates (total/avg/peak) are no longer tracked per-frame;
	// consumers should derive them from the per-frame frame_time_ms stream.
	r_metrics["total_frames_rendered"] = static_cast<int64_t>(frame_state.frame_counter);
	r_metrics["avg_frame_time_ms"] = p_frame_time_ms;
	r_metrics["avg_frame_to_frame_ms"] = p_frame_time_ms;
	r_metrics["peak_frame_time_ms"] = p_frame_time_ms;
	r_metrics["cull_projection_contract_mismatches"] = static_cast<int64_t>(0);
	r_metrics["buffer_upload_time_ms"] = 0.0f;
	r_metrics["culling_time_ms"] = p_stage_valid ? p_stage_metrics.cull.cull_time_ms : 0.0f;
	r_metrics["gpu_memory_usage_mb"] = 0.0f;
	r_metrics["uploaded_splat_count"] = static_cast<int64_t>(0);
	r_metrics["rendered_splat_count"] = static_cast<int64_t>(frame_state.visible_splat_count.load(std::memory_order_acquire));
	r_metrics["using_real_data"] = data_source_info.using_real_data;
	r_metrics["instance_sort_sync_fallback_count"] = static_cast<int64_t>(0);
	r_metrics["tile_sort_sync_fallback_count"] = static_cast<int64_t>(0);
	r_metrics["sort_sync_fallback_count"] = static_cast<int64_t>(0);
	r_metrics["sort_cached_fallback_count"] = static_cast<int64_t>(0);
	r_metrics["sort_identity_fallback_count"] = static_cast<int64_t>(0);
	r_metrics["sort_cull_order_fallback_count"] = static_cast<int64_t>(0);
	r_metrics["sort_total_route_fallback_count"] = static_cast<int64_t>(0);
	r_metrics["sort_active_algorithm"] = sorting_state.active_sort_algorithm;
	r_metrics["sort_switch_reason"] = sorting_state.sort_switch_reason;
	r_metrics["sort_override_force_cpu"] = sorting_state.override_force_cpu;
	r_metrics["sort_override_force_algorithm"] = sorting_state.override_force_algorithm;
	r_metrics["sort_override_forced_algorithm"] = sorting_state.override_forced_algorithm;
	r_metrics["async_sort_used"] = false;
	r_metrics["async_sort_waited"] = false;
	r_metrics["async_overlap_efficiency"] = 0.0f;
	r_metrics["culled_by_frustum"] = p_stage_valid ? p_stage_metrics.cull.candidate_count - p_stage_metrics.cull.visible_count : static_cast<uint32_t>(0);
	r_metrics["culled_by_distance"] = static_cast<uint32_t>(0);
	r_metrics["culled_by_screen"] = static_cast<uint32_t>(0);
	r_metrics["culled_by_importance"] = static_cast<uint32_t>(0);
	r_metrics["culling_candidate_count"] = p_stage_valid ? p_stage_metrics.cull.candidate_count : static_cast<uint32_t>(0);
	r_metrics["visible_after_culling"] = p_stage_valid ? p_stage_metrics.cull.visible_count : static_cast<uint32_t>(0);
	r_metrics["used_hierarchical_culling"] = false;
	r_metrics["sort_cache_hits"] = static_cast<int64_t>(0);
	r_metrics["sort_cache_misses"] = static_cast<int64_t>(0);

	// GPU timing: read directly from TileRenderer/rasterizer
	float gpu_utilization_pct = 0.0f;
	float gpu_frame_time_ms = 0.0f;
	float gpu_binning_ms = 0.0f;
	float gpu_raster_ms = 0.0f;
	float gpu_prefix_ms = 0.0f;
	float gpu_resolve_ms = 0.0f;
	int64_t gpu_timing_frame_serial = 0;
	int64_t gpu_timing_frames_behind = 0;
	uint32_t gpu_timeline_inflight = 0;
	uint32_t gpu_timeline_completed = 0;
	uint32_t gpu_timeline_stall_count = 0;
	float gpu_timeline_stall_ms = 0.0f;
	uint64_t gpu_timeline_last_value = 0;
	const bool has_tile_renderer = p_renderer.get_tile_renderer_state().renderer.is_valid();
	if (has_tile_renderer) {
		const TileRenderer *tr = p_renderer.get_tile_renderer_state().renderer.ptr();
		gpu_frame_time_ms = tr->get_last_gpu_frame_time_ms();
		gpu_binning_ms = tr->get_last_gpu_binning_time_ms();
		gpu_raster_ms = tr->get_last_gpu_raster_time_ms();
		gpu_prefix_ms = tr->get_last_gpu_prefix_time_ms();
		gpu_resolve_ms = tr->get_last_gpu_resolve_time_ms();
		if (subsystem_state.rasterizer.is_valid()) {
			RasterPerformance rp = subsystem_state.rasterizer->get_performance();
			gpu_timing_frame_serial = static_cast<int64_t>(rp.timing_frame_serial);
			gpu_timing_frames_behind = static_cast<int64_t>(rp.timing_frames_behind);
		}
		GPUPerformanceMonitor::SummaryMetrics timeline =
				p_renderer.get_tile_renderer_state().gpu_performance_monitor.get_summary_metrics();
		gpu_timeline_inflight = timeline.inflight_frames;
		gpu_timeline_completed = timeline.completed_frames;
		gpu_timeline_stall_count = timeline.stall_count;
		gpu_timeline_stall_ms = float(timeline.total_stall_ns) / 1000000.0f;
		gpu_timeline_last_value = timeline.last_frame_index;
		gpu_utilization_pct = p_renderer.get_tile_renderer_state().gpu_performance_monitor.get_gpu_utilization_async() * 100.0f;
	}
	r_metrics["gpu_utilization_percent"] = gpu_utilization_pct;
	r_metrics["gpu_frame_time_ms"] = gpu_frame_time_ms;
	r_metrics["gpu_tile_binning_time_ms"] = gpu_binning_ms;
	r_metrics["gpu_tile_raster_time_ms"] = gpu_raster_ms;
	r_metrics["gpu_tile_prefix_time_ms"] = gpu_prefix_ms;
	r_metrics["gpu_tile_resolve_time_ms"] = gpu_resolve_ms;
	r_metrics["gpu_timing_frame_serial"] = gpu_timing_frame_serial;
	r_metrics["gpu_timing_frames_behind"] = gpu_timing_frames_behind;
	r_metrics["gpu_timeline_inflight_frames"] = static_cast<int64_t>(gpu_timeline_inflight);
	r_metrics["gpu_timeline_completed_frames"] = static_cast<int64_t>(gpu_timeline_completed);
	r_metrics["gpu_timeline_stall_count"] = static_cast<int64_t>(gpu_timeline_stall_count);
	r_metrics["gpu_timeline_stall_ms"] = gpu_timeline_stall_ms;
	r_metrics["gpu_timeline_last_value"] = static_cast<int64_t>(gpu_timeline_last_value);
	r_metrics["stage_metrics_valid"] = p_stage_valid;
	r_metrics["stage_cull_has_visible"] = p_stage_metrics.cull.has_visible;
	r_metrics["stage_cull_visible_count"] = p_stage_metrics.cull.visible_count;
	r_metrics["stage_cull_candidate_count"] = p_stage_metrics.cull.candidate_count;
	r_metrics["stage_cull_time_ms"] = p_stage_metrics.cull.cull_time_ms;
	r_metrics["stage_cull_visible_domain"] = GaussianRenderState::index_domain_to_string(p_stage_metrics.cull.visible_domain);
	r_metrics["stage_sort_executed"] = p_stage_metrics.sort.did_sort;
	r_metrics["stage_sort_input_count"] = p_stage_metrics.sort.input_count;
	r_metrics["stage_sort_sorted_count"] = p_stage_metrics.sort.sorted_count;
	r_metrics["stage_sort_time_ms"] = p_stage_metrics.sort.sort_time_ms;
	r_metrics["stage_sort_input_domain"] = GaussianRenderState::index_domain_to_string(p_stage_metrics.sort.input_domain);
	r_metrics["stage_sort_output_domain"] = GaussianRenderState::index_domain_to_string(p_stage_metrics.sort.output_domain);
	r_metrics["stage_raster_time_ms"] = p_stage_metrics.raster.render_time_ms;
	r_metrics["stage_raster_cached"] = p_stage_metrics.raster.reused_cached_render;
	r_metrics["stage_raster_painterly"] = p_stage_metrics.raster.painterly_active;
	r_metrics["stage_composite_executed"] = p_stage_metrics.composite_executed;
	r_metrics["stage_composite_time_ms"] = p_stage_metrics.composite_time_ms;
	r_metrics["stage_cull_status"] = p_stage_valid ? _stage_status_to_string(p_stage_metrics.cull_result.status) : String("unknown");
	r_metrics["stage_cull_reason"] = p_stage_metrics.cull_result.reason;
	r_metrics["stage_cull_is_error"] = p_stage_metrics.cull_result.is_error;
	r_metrics["stage_sort_status"] = p_stage_valid ? _stage_status_to_string(p_stage_metrics.sort_result.status) : String("unknown");
	r_metrics["stage_sort_reason"] = p_stage_metrics.sort_result.reason;
	r_metrics["stage_sort_is_error"] = p_stage_metrics.sort_result.is_error;
	r_metrics["stage_raster_status"] = p_stage_valid ? _stage_status_to_string(p_stage_metrics.raster_result.status) : String("unknown");
	r_metrics["stage_raster_reason"] = p_stage_metrics.raster_result.reason;
	r_metrics["stage_raster_is_error"] = p_stage_metrics.raster_result.is_error;
	r_metrics["stage_composite_status"] = p_stage_valid ? _stage_status_to_string(p_stage_metrics.composite_result.status) : String("unknown");
	r_metrics["stage_composite_reason"] = p_stage_metrics.composite_result.reason;
	r_metrics["stage_composite_is_error"] = p_stage_metrics.composite_result.is_error;
	r_metrics["route_uid"] = debug_state.route_uid;
	r_metrics["sort_route_uid"] = debug_state.sort_route_uid;

	// Streaming telemetry: read from streaming system directly
	const auto &streaming_state_obj = p_renderer.get_streaming_state();
	if (streaming_state_obj.current_streaming_system.is_valid()) {
		const Dictionary streaming_state = streaming_state_obj.current_streaming_system->get_streaming_analytics();
		r_metrics["streaming_state"] = streaming_state;
		Dictionary streaming_diagnostics;
		if (streaming_state.has("diagnostics")) {
			streaming_diagnostics = streaming_state["diagnostics"];
			r_metrics["streaming_diagnostics"] = streaming_diagnostics;
		}
		r_metrics["streaming_diagnostics_category"] = streaming_state.get("diagnostics_category", String("ok"));
		r_metrics["streaming_diagnostics_fingerprint"] = streaming_state.get("diagnostics_fingerprint", String("ok"));
		r_metrics["streaming_diagnostics_has_failure"] = streaming_state.get("diagnostics_has_failure", false);
		r_metrics["streaming_cap_tier_preset"] = streaming_state.get("cap_tier_preset", String("custom"));
		r_metrics["streaming_cap_tier_active"] = streaming_state.get("cap_tier_active", false);
		r_metrics["streaming_effective_upload_cap_mb_per_frame"] = streaming_state.get("effective_upload_cap_mb_per_frame", int64_t(0));
		r_metrics["streaming_effective_upload_cap_mb_per_slice"] = streaming_state.get("effective_upload_cap_mb_per_slice", int64_t(0));
		r_metrics["streaming_effective_upload_cap_mb_per_second"] = streaming_state.get("effective_upload_cap_mb_per_second", int64_t(0));
		r_metrics["streaming_effective_vram_budget_mb"] = streaming_state.get("effective_vram_budget_mb", int64_t(0));
		r_metrics["streaming_effective_vram_min_chunks"] = streaming_state.get("effective_vram_min_chunks", int64_t(0));
		r_metrics["streaming_effective_vram_max_chunks"] = streaming_state.get("effective_vram_max_chunks", int64_t(0));
		r_metrics["streaming_cap_source_upload_mb_per_frame"] = streaming_state.get("cap_source_upload_mb_per_frame", String("project_default"));
		r_metrics["streaming_cap_source_upload_mb_per_slice"] = streaming_state.get("cap_source_upload_mb_per_slice", String("project_default"));
		r_metrics["streaming_cap_source_upload_mb_per_second"] = streaming_state.get("cap_source_upload_mb_per_second", String("project_default"));
		r_metrics["streaming_cap_source_vram_budget_mb"] = streaming_state.get("cap_source_vram_budget_mb", String("project_default"));
		r_metrics["streaming_cap_source_vram_min_chunks"] = streaming_state.get("cap_source_vram_min_chunks", String("project_default"));
		r_metrics["streaming_cap_source_vram_max_chunks"] = streaming_state.get("cap_source_vram_max_chunks", String("project_default"));
		r_metrics["streaming_upload_frame_cap_hit"] = streaming_state.get("upload_frame_cap_hit", false);
		r_metrics["streaming_upload_slice_cap_hit"] = streaming_state.get("upload_slice_cap_hit", false);
		r_metrics["streaming_upload_bandwidth_cap_hit"] = streaming_state.get("upload_bandwidth_cap_hit", false);
		r_metrics["streaming_chunk_load_cap_hit"] = streaming_state.get("chunk_load_cap_hit", false);
		r_metrics["streaming_vram_chunk_cap_hit"] = streaming_state.get("vram_chunk_cap_hit", false);
		r_metrics["streaming_queue_pressure_active"] = streaming_state.get("queue_pressure_active", false);
		r_metrics["streaming_queue_pressure_frames"] = streaming_diagnostics.get("queue_pressure_frames", int64_t(0));
		r_metrics["streaming_vram_cap_hit_frames"] = streaming_diagnostics.get("vram_cap_hit_frames", int64_t(0));
	} else {
		r_metrics["streaming_diagnostics_category"] = String("unknown");
		r_metrics["streaming_diagnostics_fingerprint"] = String("unavailable");
		r_metrics["streaming_diagnostics_has_failure"] = false;
		r_metrics["streaming_cap_tier_preset"] = String("custom");
		r_metrics["streaming_cap_tier_active"] = false;
		r_metrics["streaming_effective_upload_cap_mb_per_frame"] = static_cast<int64_t>(0);
		r_metrics["streaming_effective_upload_cap_mb_per_slice"] = static_cast<int64_t>(0);
		r_metrics["streaming_effective_upload_cap_mb_per_second"] = static_cast<int64_t>(0);
		r_metrics["streaming_effective_vram_budget_mb"] = static_cast<int64_t>(0);
		r_metrics["streaming_effective_vram_min_chunks"] = static_cast<int64_t>(0);
		r_metrics["streaming_effective_vram_max_chunks"] = static_cast<int64_t>(0);
		r_metrics["streaming_cap_source_upload_mb_per_frame"] = String("project_default");
		r_metrics["streaming_cap_source_upload_mb_per_slice"] = String("project_default");
		r_metrics["streaming_cap_source_upload_mb_per_second"] = String("project_default");
		r_metrics["streaming_cap_source_vram_budget_mb"] = String("project_default");
		r_metrics["streaming_cap_source_vram_min_chunks"] = String("project_default");
		r_metrics["streaming_cap_source_vram_max_chunks"] = String("project_default");
		r_metrics["streaming_upload_frame_cap_hit"] = false;
		r_metrics["streaming_upload_slice_cap_hit"] = false;
		r_metrics["streaming_upload_bandwidth_cap_hit"] = false;
		r_metrics["streaming_chunk_load_cap_hit"] = false;
		r_metrics["streaming_vram_chunk_cap_hit"] = false;
		r_metrics["streaming_queue_pressure_active"] = false;
		r_metrics["streaming_queue_pressure_frames"] = static_cast<int64_t>(0);
		r_metrics["streaming_vram_cap_hit_frames"] = static_cast<int64_t>(0);
	}

	if (subsystem_state.rasterizer.is_valid()) {
		Vector2i tile_grid = subsystem_state.rasterizer->get_tile_grid_size();
		r_metrics["tile_grid_size"] = tile_grid;
		r_metrics["tile_size"] = subsystem_state.rasterizer->get_tile_size();
		RasterStats raster_stats = subsystem_state.rasterizer->get_render_stats();
		r_metrics["overlap_records"] = static_cast<int64_t>(raster_stats.overlap_records);
		// Note: "overlap_record_budget" is now provided by the diagnostics snapshot
		// (via overlap_record_budget in GaussianSplatDiagnosticsSnapshot::to_dictionary()).
		// Only inject the extended budget keys that are NOT in the snapshot.
		r_metrics["overlap_record_budget_effective"] = static_cast<int64_t>(raster_stats.overlap_record_budget_effective);
		r_metrics["overlap_record_budget_configured"] = static_cast<int64_t>(raster_stats.overlap_record_budget_configured);
		r_metrics["overlap_thinning_keep_ratio"] = raster_stats.overlap_thinning_keep_ratio;
		r_metrics["sorted_indices_blend_fallback_active"] = raster_stats.sorted_indices_blend_fallback_active;
		r_metrics["sorted_indices_blend_fallback_reason"] = raster_stats.sorted_indices_blend_fallback_reason;
	} else {
		r_metrics["tile_grid_size"] = Vector2i(0, 0);
		r_metrics["tile_size"] = 0;
		r_metrics["overlap_records"] = static_cast<int64_t>(0);
		// Note: "overlap_record_budget" is now provided by the diagnostics snapshot.
		r_metrics["overlap_record_budget_effective"] = static_cast<int64_t>(0);
		r_metrics["overlap_record_budget_configured"] = static_cast<int64_t>(0);
		r_metrics["overlap_thinning_keep_ratio"] = 1.0f;
		r_metrics["sorted_indices_blend_fallback_active"] = false;
		r_metrics["sorted_indices_blend_fallback_reason"] = String();
	}
}

static Dictionary _validate_production_metrics(const Dictionary &p_metrics) {
	Array issues;
	Array contract = _production_metrics_contract();
	for (int i = 0; i < contract.size(); i++) {
		const String key = contract[i];
		if (!p_metrics.has(key)) {
			issues.push_back(vformat("missing:%s", key));
		}
	}

	const int64_t visible_splats = p_metrics.get("visible_splats", int64_t(-1));
	const int64_t total_splats = p_metrics.get("total_splats", int64_t(-1));
	if (visible_splats < 0) {
		issues.push_back("visible_splats_negative");
	}
	if (total_splats < 0) {
		issues.push_back("total_splats_negative");
	}
	// NOTE: visible_splats CAN exceed total_splats with instance pipeline because:
	// - visible_splats is aggregated across all instances
	// - total_splats is from a single GaussianData asset
	// - overlap rendering duplicates splats across tile boundaries
	// This is expected behavior, not a contract violation.

	const float cull_ms = static_cast<float>(p_metrics.get("cull_ms", -1.0f));
	const float sort_ms = static_cast<float>(p_metrics.get("sort_ms", -1.0f));
	const float raster_ms = static_cast<float>(p_metrics.get("raster_ms", -1.0f));
	const float composite_ms = static_cast<float>(p_metrics.get("composite_ms", -1.0f));
	const float stage_total_ms = static_cast<float>(p_metrics.get("stage_total_ms", -1.0f));
	const float render_ms = static_cast<float>(p_metrics.get("render_ms", -1.0f));
	const float frame_time_ms = static_cast<float>(p_metrics.get("frame_time_ms", -1.0f));
	const float gpu_frame_ms = static_cast<float>(p_metrics.get("gpu_frame_ms", -1.0f));
	const float gpu_binning_ms = static_cast<float>(p_metrics.get("gpu_binning_ms", -1.0f));
	const float gpu_prefix_ms = static_cast<float>(p_metrics.get("gpu_prefix_ms", -1.0f));
	const float gpu_raster_ms = static_cast<float>(p_metrics.get("gpu_raster_ms", -1.0f));
	const float gpu_resolve_ms = static_cast<float>(p_metrics.get("gpu_resolve_ms", -1.0f));
	const int64_t gpu_timing_frame_serial = p_metrics.get("gpu_timing_frame_serial", int64_t(-1));
	const int64_t gpu_timing_frames_behind = p_metrics.get("gpu_timing_frames_behind", int64_t(-1));

	if (cull_ms < 0.0f || !_is_finite(cull_ms)) {
		issues.push_back("cull_ms_invalid");
	}
	if (sort_ms < 0.0f || !_is_finite(sort_ms)) {
		issues.push_back("sort_ms_invalid");
	}
	if (raster_ms < 0.0f || !_is_finite(raster_ms)) {
		issues.push_back("raster_ms_invalid");
	}
	if (composite_ms < 0.0f || !_is_finite(composite_ms)) {
		issues.push_back("composite_ms_invalid");
	}
	if (stage_total_ms < 0.0f || !_is_finite(stage_total_ms)) {
		issues.push_back("stage_total_ms_invalid");
	}
	if (render_ms < 0.0f || !_is_finite(render_ms)) {
		issues.push_back("render_ms_invalid");
	}
	if (frame_time_ms < 0.0f || !_is_finite(frame_time_ms)) {
		issues.push_back("frame_time_ms_invalid");
	}
	if (gpu_frame_ms < 0.0f || !_is_finite(gpu_frame_ms)) {
		issues.push_back("gpu_frame_ms_invalid");
	}
	if (gpu_binning_ms < 0.0f || !_is_finite(gpu_binning_ms)) {
		issues.push_back("gpu_binning_ms_invalid");
	}
	if (gpu_prefix_ms < 0.0f || !_is_finite(gpu_prefix_ms)) {
		issues.push_back("gpu_prefix_ms_invalid");
	}
	if (gpu_raster_ms < 0.0f || !_is_finite(gpu_raster_ms)) {
		issues.push_back("gpu_raster_ms_invalid");
	}
	if (gpu_resolve_ms < 0.0f || !_is_finite(gpu_resolve_ms)) {
		issues.push_back("gpu_resolve_ms_invalid");
	}
	if (gpu_timing_frame_serial < 0) {
		issues.push_back("gpu_timing_frame_serial_invalid");
	}
	if (gpu_timing_frames_behind < 0) {
		issues.push_back("gpu_timing_frames_behind_invalid");
	}

	const bool stage_valid = bool(p_metrics.get("stage_metrics_valid", false));
	if (!stage_valid) {
		issues.push_back("stage_metrics_invalid");
	}
	const String route_uid = p_metrics.get("route_uid", String());
	const String sort_route_uid = p_metrics.get("sort_route_uid", String());
	const bool route_no_device = route_uid == String(RenderRouteUID::COMMON_FAIL_NO_DEVICE);
	if (stage_valid && route_uid.is_empty()) {
		issues.push_back("route_uid_empty");
	}
	// No-device fallback can legitimately skip sort-route assignment.
	if (stage_valid && !route_no_device && sort_route_uid.is_empty()) {
		issues.push_back("sort_route_uid_empty");
	}
	const String stage_cull_status = p_metrics.get("stage_cull_status", String("unknown"));
	const String stage_sort_status = p_metrics.get("stage_sort_status", String("unknown"));
	const String stage_raster_status = p_metrics.get("stage_raster_status", String("unknown"));
	if (!route_uid.is_empty() && !sort_route_uid.is_empty()) {
		const bool sort_route_no_device = sort_route_uid == String(RenderRouteUID::COMMON_FAIL_NO_DEVICE);
		if (route_no_device != sort_route_no_device) {
			issues.push_back("route_sort_route_device_mismatch");
		}
		if (stage_sort_status == "success" && sort_route_uid.begins_with("COMMON.FAIL.")) {
			issues.push_back("sort_route_uid_status_mismatch");
		}
	}
	const bool has_meaningful_workload = MAX(visible_splats, total_splats) >= 1024;
	if (stage_valid && has_meaningful_workload && total_splats > 0 && stage_cull_status == "success" && cull_ms <= 0.0f) {
		issues.push_back("cull_ms_placeholder");
	}
	if (stage_valid && has_meaningful_workload && visible_splats > 0 && stage_sort_status == "success" && sort_ms <= 0.0f) {
		issues.push_back("sort_ms_placeholder");
	}
	if (stage_valid && has_meaningful_workload && visible_splats > 0 && stage_raster_status == "success" && raster_ms <= 0.0f) {
		issues.push_back("raster_ms_placeholder");
	}
	const bool has_gpu_breakdown = gpu_binning_ms > 0.0f ||
			gpu_prefix_ms > 0.0f ||
			gpu_raster_ms > 0.0f ||
			gpu_resolve_ms > 0.0f;
	if (stage_valid && has_meaningful_workload && visible_splats > 0 &&
			stage_raster_status == "success" &&
			gpu_timing_frame_serial > 0 &&
			gpu_frame_ms > 0.0f &&
			!has_gpu_breakdown) {
		issues.push_back("gpu_pass_timing_placeholder");
	}

	const String data_source = p_metrics.get("data_source", String());
	if (data_source.is_empty()) {
		issues.push_back("data_source_empty");
	}
	const String raster_path = p_metrics.get("raster_path", String());
	if (raster_path.is_empty()) {
		issues.push_back("raster_path_empty");
	}

	String issues_text;
	for (int i = 0; i < issues.size(); i++) {
		if (i > 0) {
			issues_text += ", ";
		}
		const String issue = issues[i];
		issues_text += issue;
	}
	if (!issues.is_empty()) {
		GS_LOG_WARN_DEFAULT(vformat("[Diagnostics] Production metrics contract violated: %s", issues_text));
#ifdef DEV_ENABLED
		WARN_PRINT(vformat("Production metrics contract violated: %s", issues_text));
#endif
	}

	Dictionary validation;
	validation["valid"] = issues.is_empty();
	validation["issues"] = issues;
	validation["frame"] = p_metrics.get("frame", int64_t(0));
	return validation;
}

static Dictionary _evaluate_perf_gate(const ProductionMetricsConfig &p_config, const Dictionary &p_metrics) {
	Dictionary result;
	result["enabled"] = p_config.perf_gate_enabled;
	result["applicable"] = false;
	result["passed"] = true;
	result["budget_ms"] = p_config.perf_gate_budget_ms;
	result["splat_threshold"] = static_cast<int64_t>(p_config.perf_gate_splat_threshold);

	if (!p_config.perf_gate_enabled) {
		result["reason"] = "disabled";
		return result;
	}

	const bool stage_valid = bool(p_metrics.get("stage_metrics_valid", false));
	const int64_t visible_splats = p_metrics.get("visible_splats", int64_t(0));
	const float stage_total_ms = static_cast<float>(p_metrics.get("stage_total_ms", 0.0f));
	result["visible_splats"] = visible_splats;
	result["stage_total_ms"] = stage_total_ms;

	if (!stage_valid) {
		result["reason"] = "stage_metrics_invalid";
		return result;
	}
	if (visible_splats < static_cast<int64_t>(p_config.perf_gate_splat_threshold)) {
		result["reason"] = "below_splat_threshold";
		return result;
	}

	result["applicable"] = true;
	const bool passed = stage_total_ms <= p_config.perf_gate_budget_ms;
	result["passed"] = passed;
	result["reason"] = passed ? "within_budget" : "budget_exceeded";
	result["over_budget_ms"] = passed ? 0.0f : (stage_total_ms - p_config.perf_gate_budget_ms);
	return result;
}

static void _update_production_summary(GaussianSplatRenderer::DiagnosticsState &p_state,
		const ProductionMetricsConfig &p_config,
		const Dictionary &p_metrics,
		const Dictionary &p_perf_gate_result,
		uint64_t p_frame_end_usec) {
	const uint64_t frame = static_cast<uint64_t>(p_metrics.get("frame", int64_t(0)));
	if (p_state.production_metrics_window_frames == 0) {
		p_state.production_metrics_window_start_frame = frame;
		p_state.production_metrics_window_start_usec = p_frame_end_usec;
	}

	p_state.production_metrics_window_frames++;

	const float render_ms = static_cast<float>(p_metrics.get("render_ms", 0.0f));
	const float frame_time_ms = static_cast<float>(p_metrics.get("frame_time_ms", 0.0f));
	const float cull_ms = static_cast<float>(p_metrics.get("cull_ms", 0.0f));
	const float sort_ms = static_cast<float>(p_metrics.get("sort_ms", 0.0f));
	const float raster_ms = static_cast<float>(p_metrics.get("raster_ms", 0.0f));
	const float composite_ms = static_cast<float>(p_metrics.get("composite_ms", 0.0f));
	const float stage_total_ms = static_cast<float>(p_metrics.get("stage_total_ms", 0.0f));
	const uint32_t visible_splats = static_cast<uint32_t>(static_cast<int64_t>(p_metrics.get("visible_splats", int64_t(0))));

	p_state.production_metrics_frame_ms_sum += static_cast<double>(frame_time_ms > 0.0f ? frame_time_ms : render_ms);
	p_state.production_metrics_cull_ms_sum += static_cast<double>(cull_ms);
	p_state.production_metrics_sort_ms_sum += static_cast<double>(sort_ms);
	p_state.production_metrics_raster_ms_sum += static_cast<double>(raster_ms);
	p_state.production_metrics_composite_ms_sum += static_cast<double>(composite_ms);
	p_state.production_metrics_stage_total_ms_sum += static_cast<double>(stage_total_ms);
	p_state.production_metrics_frame_ms_peak = MAX(p_state.production_metrics_frame_ms_peak,
			static_cast<double>(frame_time_ms > 0.0f ? frame_time_ms : render_ms));
	p_state.production_metrics_stage_ms_peak = MAX(p_state.production_metrics_stage_ms_peak, static_cast<double>(stage_total_ms));
	p_state.production_metrics_visible_peak = MAX(p_state.production_metrics_visible_peak, visible_splats);
	p_state.production_metrics_visible_sum += visible_splats;

	if (bool(p_perf_gate_result.get("enabled", false)) && bool(p_perf_gate_result.get("applicable", false))) {
		p_state.production_metrics_perf_gate_checks++;
		if (!bool(p_perf_gate_result.get("passed", true))) {
			p_state.production_metrics_perf_gate_failures++;
		}
	}

	if (p_state.production_metrics_window_frames < p_config.summary_interval_frames) {
		return;
	}

	const double frame_count = static_cast<double>(p_state.production_metrics_window_frames);
	Dictionary summary;
	summary["start_frame"] = static_cast<int64_t>(p_state.production_metrics_window_start_frame);
	summary["end_frame"] = static_cast<int64_t>(frame);
	summary["frame_count"] = static_cast<int64_t>(p_state.production_metrics_window_frames);
	summary["window_start_usec"] = static_cast<int64_t>(p_state.production_metrics_window_start_usec);
	summary["window_end_usec"] = static_cast<int64_t>(p_frame_end_usec);
	summary["avg_frame_ms"] = frame_count > 0.0 ? p_state.production_metrics_frame_ms_sum / frame_count : 0.0;
	summary["avg_cull_ms"] = frame_count > 0.0 ? p_state.production_metrics_cull_ms_sum / frame_count : 0.0;
	summary["avg_sort_ms"] = frame_count > 0.0 ? p_state.production_metrics_sort_ms_sum / frame_count : 0.0;
	summary["avg_raster_ms"] = frame_count > 0.0 ? p_state.production_metrics_raster_ms_sum / frame_count : 0.0;
	summary["avg_composite_ms"] = frame_count > 0.0 ? p_state.production_metrics_composite_ms_sum / frame_count : 0.0;
	summary["avg_stage_total_ms"] = frame_count > 0.0 ? p_state.production_metrics_stage_total_ms_sum / frame_count : 0.0;
	summary["peak_frame_ms"] = p_state.production_metrics_frame_ms_peak;
	summary["peak_stage_total_ms"] = p_state.production_metrics_stage_ms_peak;
	summary["max_visible_splats"] = static_cast<int64_t>(p_state.production_metrics_visible_peak);
	summary["avg_visible_splats"] = frame_count > 0.0
			? static_cast<double>(p_state.production_metrics_visible_sum) / frame_count
			: 0.0;
	summary["perf_gate_checks"] = static_cast<int64_t>(p_state.production_metrics_perf_gate_checks);
	summary["perf_gate_failures"] = static_cast<int64_t>(p_state.production_metrics_perf_gate_failures);

	p_state.production_metrics_summaries.push_back(summary);
	while (p_state.production_metrics_summaries.size() > p_config.summary_history_size) {
		p_state.production_metrics_summaries.remove_at(0);
	}

	p_state.production_metrics_window_start_frame = 0;
	p_state.production_metrics_window_start_usec = 0;
	p_state.production_metrics_window_frames = 0;
	p_state.production_metrics_frame_ms_sum = 0.0;
	p_state.production_metrics_cull_ms_sum = 0.0;
	p_state.production_metrics_sort_ms_sum = 0.0;
	p_state.production_metrics_raster_ms_sum = 0.0;
	p_state.production_metrics_composite_ms_sum = 0.0;
	p_state.production_metrics_stage_total_ms_sum = 0.0;
	p_state.production_metrics_frame_ms_peak = 0.0;
	p_state.production_metrics_stage_ms_peak = 0.0;
	p_state.production_metrics_visible_peak = 0;
	p_state.production_metrics_visible_sum = 0;
	p_state.production_metrics_perf_gate_checks = 0;
	p_state.production_metrics_perf_gate_failures = 0;
}
} // namespace

RenderDiagnosticsOrchestrator::RenderDiagnosticsOrchestrator(GaussianSplatRenderer *p_renderer,
		RenderDebugStateOrchestrator *p_debug_state_orchestrator,
		BuildDeviceCapabilityReportFn p_build_device_capability_report) :
		renderer(p_renderer),
		debug_state_orchestrator(p_debug_state_orchestrator),
		build_device_capability_report(p_build_device_capability_report) {
	ERR_FAIL_NULL(renderer);
	ERR_FAIL_NULL(debug_state_orchestrator);
	ERR_FAIL_COND_MSG(!build_device_capability_report, "RenderDiagnosticsOrchestrator requires device capability callback.");
}

void RenderDiagnosticsOrchestrator::record_rendering_error(const RenderingError &p_error) {
	diagnostics_state.runtime_error_statistics.total_errors++;
	if (p_error.get_severity() == RenderingError::Severity::WARNING) {
		diagnostics_state.runtime_error_statistics.total_warnings++;
	}
	diagnostics_state.runtime_error_statistics.last_error = p_error;
	diagnostics_state.runtime_error_statistics.last_error_time_usec = OS::get_singleton()->get_ticks_usec();
	diagnostics_state.runtime_error_statistics.last_error_context = p_error.get_context();
	diagnostics_state.runtime_error_statistics.error_code_counts[p_error.get_code().id]++;
	diagnostics_state.runtime_error_statistics.recent_errors.push_back(p_error);
	if (diagnostics_state.runtime_error_statistics.recent_errors.size() > 16) {
		diagnostics_state.runtime_error_statistics.recent_errors.remove_at(0);
	}
	diagnostics_state.runtime_diagnostics_requested = true;
	GaussianSplatRenderer::ErrorRecoveryStateMachine::State next_state =
			diagnostics_state.recovery_state_machine.state == GaussianSplatRenderer::ErrorRecoveryStateMachine::State::DISABLED
			? GaussianSplatRenderer::ErrorRecoveryStateMachine::State::DISABLED
			: GaussianSplatRenderer::ErrorRecoveryStateMachine::State::DIAGNOSTIC;
	transition_recovery_state(next_state, p_error.get_message());
	GaussianRenderingDiagnostics::ensure_singleton();
	if (GaussianRenderingDiagnostics::get_singleton()) {
		GaussianRenderingDiagnostics::get_singleton()->notify_error(renderer, p_error);
	}
}

void RenderDiagnosticsOrchestrator::transition_recovery_state(GaussianSplatRenderer::ErrorRecoveryStateMachine::State p_state,
		const String &p_reason) {
	if (diagnostics_state.recovery_state_machine.state == p_state &&
			diagnostics_state.recovery_state_machine.reason == p_reason) {
		return;
	}
	diagnostics_state.recovery_state_machine.state = p_state;
	diagnostics_state.recovery_state_machine.reason = p_reason;
	diagnostics_state.recovery_state_machine.last_transition_frame = renderer->get_frame_state().frame_counter;
	diagnostics_state.recovery_state_machine.last_transition_time_usec = OS::get_singleton()->get_ticks_usec();
}

void RenderDiagnosticsOrchestrator::record_cross_device_operation(
		const GaussianSplatRenderer::CrossDeviceOperation &p_operation) {
	diagnostics_state.cross_device_operations.push_back(p_operation);
	const int MAX_OPS = 64;
	while (diagnostics_state.cross_device_operations.size() > MAX_OPS) {
		diagnostics_state.cross_device_operations.remove_at(0);
	}
}

void RenderDiagnosticsOrchestrator::capture_frame_timing_sample() {
	GaussianSplatRenderer::FrameTimingSample sample;
	sample.timestamp_usec = OS::get_singleton()->get_ticks_usec();
	sample.frame = renderer->get_frame_state().frame_counter;
	// Timing data now comes from StageMetrics / DiagnosticsSnapshot.
	const bool sm_valid = renderer->get_debug_state().last_stage_metrics_valid;
	const auto &sm = renderer->get_debug_state().last_stage_metrics;
	sample.render_ms = sm_valid ? sm.raster.render_time_ms : 0.0f;
	sample.sort_ms = sm_valid ? sm.sort.sort_time_ms : 0.0f;
	sample.total_ms = sample.render_ms + sample.sort_ms;
	sample.visible_splats = renderer->get_frame_state().visible_splat_count.load(std::memory_order_acquire);
	sample.used_gpu = renderer->get_sorting_state().gpu_sorter.is_valid();
	diagnostics_state.frame_timing_history.push_back(sample);
	const int MAX_SAMPLES = 240;
	while (diagnostics_state.frame_timing_history.size() > MAX_SAMPLES) {
		diagnostics_state.frame_timing_history.remove_at(0);
	}
}

void RenderDiagnosticsOrchestrator::increment_frame_counter() {
	capture_frame_timing_sample();
	renderer->get_frame_state().frame_counter++;
	GaussianRenderingDiagnostics::ensure_singleton();
	if (GaussianRenderingDiagnostics::get_singleton()) {
		GaussianRenderingDiagnostics::get_singleton()->notify_frame_completed(renderer);
	}
	emit_runtime_diagnostics_if_requested();
}

void RenderDiagnosticsOrchestrator::emit_runtime_diagnostics_if_requested() {
	if (!diagnostics_state.runtime_diagnostics_requested) {
		return;
	}
	GaussianRenderingDiagnostics::ensure_singleton();
	if (GaussianRenderingDiagnostics::get_singleton()) {
		GaussianRenderingDiagnostics::get_singleton()->request_runtime_report();
	}
	diagnostics_state.runtime_diagnostics_requested = false;
}

Array RenderDiagnosticsOrchestrator::serialize_texture_trace() const {
	Array result;
	for (const GaussianSplatRenderer::TextureTraceEntry &entry : diagnostics_state.texture_allocation_trace) {
		Dictionary dict;
		dict["timestamp_usec"] = static_cast<int64_t>(entry.timestamp_usec);
		dict["action"] = entry.action;
		dict["texture_rid"] = static_cast<int64_t>(entry.texture_rid);
		dict["device_instance_id"] = static_cast<int64_t>(entry.device_instance_id);
		dict["format"] = entry.format_label;
		dict["width"] = entry.extent.x;
		dict["height"] = entry.extent.y;
		result.push_back(dict);
	}
	return result;
}

Array RenderDiagnosticsOrchestrator::serialize_cross_device_operations() const {
	Array result;
	for (const GaussianSplatRenderer::CrossDeviceOperation &op : diagnostics_state.cross_device_operations) {
		Dictionary dict;
		dict["timestamp_usec"] = static_cast<int64_t>(op.timestamp_usec);
		dict["context"] = op.context;
		dict["source_device"] = static_cast<int64_t>(op.source_device);
		dict["target_device"] = static_cast<int64_t>(op.target_device);
		result.push_back(dict);
	}
	return result;
}

Array RenderDiagnosticsOrchestrator::serialize_frame_timing() const {
	Array result;
	for (const GaussianSplatRenderer::FrameTimingSample &sample : diagnostics_state.frame_timing_history) {
		Dictionary dict;
		dict["timestamp_usec"] = static_cast<int64_t>(sample.timestamp_usec);
		dict["frame"] = static_cast<int64_t>(sample.frame);
		dict["render_ms"] = sample.render_ms;
		dict["sort_ms"] = sample.sort_ms;
		dict["total_ms"] = sample.total_ms;
		dict["visible_splats"] = static_cast<int64_t>(sample.visible_splats);
		dict["used_gpu"] = sample.used_gpu;
		result.push_back(dict);
	}
	return result;
}

Dictionary RenderDiagnosticsOrchestrator::serialize_error_statistics() const {
	Dictionary dict;
	dict["total_errors"] = static_cast<int64_t>(diagnostics_state.runtime_error_statistics.total_errors);
	dict["total_warnings"] = static_cast<int64_t>(diagnostics_state.runtime_error_statistics.total_warnings);
	dict["total_recoveries"] = static_cast<int64_t>(diagnostics_state.runtime_error_statistics.total_recoveries);
	dict["last_error_time_usec"] = static_cast<int64_t>(diagnostics_state.runtime_error_statistics.last_error_time_usec);
	dict["last_recovery_time_usec"] = static_cast<int64_t>(diagnostics_state.runtime_error_statistics.last_recovery_time_usec);
	dict["last_error"] = diagnostics_state.runtime_error_statistics.last_error.to_dictionary();
	dict["last_error_context"] = diagnostics_state.runtime_error_statistics.last_error_context;

	Dictionary error_counts;
	for (const KeyValue<int, uint64_t> &kv : diagnostics_state.runtime_error_statistics.error_code_counts) {
		error_counts[String::num_int64(kv.key)] = static_cast<int64_t>(kv.value);
	}
	dict["error_code_counts"] = error_counts;

	Dictionary recovery_counts;
	for (const KeyValue<int, uint64_t> &kv : diagnostics_state.runtime_error_statistics.recovery_code_counts) {
		recovery_counts[String::num_int64(kv.key)] = static_cast<int64_t>(kv.value);
	}
	dict["recovery_code_counts"] = recovery_counts;

	Array history;
	for (const RenderingError &error : diagnostics_state.runtime_error_statistics.recent_errors) {
		history.push_back(error.to_dictionary());
	}
	dict["recent_errors"] = history;
	dict["recovery_state"] = static_cast<int64_t>(diagnostics_state.recovery_state_machine.state);
	dict["recovery_reason"] = diagnostics_state.recovery_state_machine.reason;
	dict["recovery_transition_frame"] = static_cast<int64_t>(diagnostics_state.recovery_state_machine.last_transition_frame);
	dict["recovery_transition_time_usec"] = static_cast<int64_t>(diagnostics_state.recovery_state_machine.last_transition_time_usec);
	return dict;
}

Dictionary RenderDiagnosticsOrchestrator::build_render_stats() const {
	// -------------------------------------------------------------------
	// Step 1: Start from the canonical diagnostics snapshot.
	// This is the single source of truth for per-frame timing, stats,
	// and metadata.  Keys emitted here should NOT be re-added below.
	// -------------------------------------------------------------------
	const GaussianSplatDiagnosticsSnapshot &snap = renderer->get_diagnostics_snapshot();
	Dictionary stats = snap.to_dictionary();

	// -------------------------------------------------------------------
	// Step 2: Rebuild overlays if dirty (side-effect on mutable renderer).
	// -------------------------------------------------------------------
	GaussianSplatRenderer *mutable_renderer = const_cast<GaussianSplatRenderer *>(renderer);
	if (mutable_renderer->get_debug_state().overlay_dirty) {
		if (mutable_renderer->get_subsystem_state().debug_overlay_system.is_valid()) {
			mutable_renderer->get_subsystem_state().debug_overlay_system->rebuild_renderer_overlay_statistics_from_cache(mutable_renderer);
		}
	}
	// -------------------------------------------------------------------
	// Step 3: Merge the telemetry extras (streaming, VRAM, LOD, culling
	// breakdown, GPU timeline, etc.) that live outside the snapshot.
	// When the full telemetry snapshot exists we merge it first so that
	// _append_telemetry_extras can overwrite with fresh values.
	// -------------------------------------------------------------------
	if (!diagnostics_state.last_telemetry_snapshot.is_empty()) {
		// Merge telemetry, but skip keys the snapshot already provides
		// to avoid stale data overwriting the canonical source.
		Array telemetry_keys = diagnostics_state.last_telemetry_snapshot.keys();
		for (int i = 0; i < telemetry_keys.size(); i++) {
			const Variant &key = telemetry_keys[i];
			if (!stats.has(key)) {
				stats[key] = diagnostics_state.last_telemetry_snapshot[key];
			}
		}
	} else {
		// Fallback: populate the basics that consumers expect when no
		// telemetry snapshot is available.
		stats["total_splats"] = renderer->get_scene_state().gaussian_data.is_valid() ? renderer->get_scene_state().gaussian_data->get_count() : 0;
		stats["frame_count"] = renderer->get_frame_state().frame_counter;
		stats["render_mode"] = renderer->get_render_config().render_mode;
		const bool sm_valid = mutable_renderer->get_debug_state().last_stage_metrics_valid;
		GaussianSplatRenderer::StageMetrics stage_metrics = sm_valid
				? mutable_renderer->get_debug_state().last_stage_metrics
				: GaussianSplatRenderer::StageMetrics();
		float fallback_render_ms = sm_valid ? stage_metrics.raster.render_time_ms : 0.0f;
		_append_telemetry_extras(*mutable_renderer, stage_metrics, sm_valid,
				fallback_render_ms, stats);
	}

	// -------------------------------------------------------------------
	// Step 4: Additional keys NOT in the snapshot and NOT in telemetry.
	// -------------------------------------------------------------------

	// Painterly config
	stats["painterly_enabled"] = renderer->get_painterly_config().enabled;
	stats["painterly_low_end_mode"] = renderer->get_painterly_config().low_end_mode;
	PainterlyPassGraph *pass_graph = renderer->get_subsystem_state().painterly_renderer.is_valid()
			? renderer->get_subsystem_state().painterly_renderer->get_pass_graph()
			: nullptr;
	stats["painterly_internal_scale"] = pass_graph ? pass_graph->get_internal_scale() : renderer->get_painterly_config().internal_scale;

	// Camera / view state
	stats["using_scene_data_camera"] = renderer->get_view_state().using_scene_data;
	stats["debug_cam_origin_x"] = renderer->get_view_state().last_camera_to_world_transform.origin.x;
	stats["debug_cam_origin_y"] = renderer->get_view_state().last_camera_to_world_transform.origin.y;
	stats["debug_cam_origin_z"] = renderer->get_view_state().last_camera_to_world_transform.origin.z;
	stats["debug_cam_basis_00"] = renderer->get_view_state().last_camera_to_world_transform.basis[0][0];

	// Binning debug counters
	if (debug_state_orchestrator) {
		const GaussianSplatRenderer::DebugConfig &debug_config = renderer->get_debug_config();
		if (debug_config.enable_binning_counters || debug_config.dump_gpu_counters) {
			const Dictionary binning = debug_state_orchestrator->get_binning_debug_counters();
			if (!binning.is_empty()) {
				stats["sh_cache_hits"] = binning.get("sh_cache_hits", 0);
				stats["sh_cache_updates"] = binning.get("sh_cache_updates", 0);
				stats["sh_cache_forced_updates"] = binning.get("sh_cache_forced_updates", 0);
				stats["sh_cache_hit_rate"] = binning.get("sh_cache_hit_rate", 0.0);
			}
		}
	}

	// Sorted indices preview
	PackedInt32Array sorted_preview;
	if (renderer->get_subsystem_state().gpu_culler.is_valid()) {
		int preview_count = MIN((int)renderer->get_subsystem_state().gpu_culler->get_state().culled_indices.size(), 32);
		if (preview_count > 0) {
			sorted_preview.resize(preview_count);
			for (int i = 0; i < preview_count; i++) {
				sorted_preview.set(i, (int)renderer->get_subsystem_state().gpu_culler->get_state().culled_indices[i]);
			}
		}
	}
	if (sorted_preview.is_empty() && renderer->get_subsystem_state().sorting_pipeline.is_valid()) {
		const uint32_t sorted_count = renderer->get_sorting_state().sorted_splat_count;
		const int preview_count = MIN((int)sorted_count, 32);
		if (preview_count > 0) {
			RID sort_indices_buffer = renderer->get_subsystem_state().sorting_pipeline->get_sort_indices_buffer();
			RenderingDevice *fallback_device = renderer->get_device_state().rd;
			RenderingDevice *owner = renderer->get_resource_owner(sort_indices_buffer, fallback_device);
			if (!owner) {
				owner = fallback_device;
			}
			if (owner && sort_indices_buffer.is_valid()) {
				Vector<uint8_t> preview_bytes = owner->buffer_get_data(sort_indices_buffer, 0,
						uint32_t(preview_count) * uint32_t(sizeof(uint32_t)));
				const int expected_bytes = preview_count * int(sizeof(uint32_t));
				if (preview_bytes.size() >= expected_bytes) {
					sorted_preview.resize(preview_count);
					const uint32_t *preview_indices = reinterpret_cast<const uint32_t *>(preview_bytes.ptr());
					for (int i = 0; i < preview_count; i++) {
						sorted_preview.set(i, int(preview_indices[i]));
					}
				}
			}
		}
	}
	stats["sorted_indices_preview"] = sorted_preview;

	// GPU sorter state and metrics
	const bool has_gpu_sorter = renderer->get_sorting_state().gpu_sorter.is_valid();
	stats["gpu_sorter_initialized"] = has_gpu_sorter;
	stats["gpu_sorter_async_pipeline_ready"] = false;
	stats["rendering_device_ready"] = renderer->get_device_state().rd != nullptr;
	stats["sort_active_algorithm"] = renderer->get_sorting_state().active_sort_algorithm;
	stats["sort_switch_reason"] = renderer->get_sorting_state().sort_switch_reason;
	stats["sort_override_force_cpu"] = renderer->get_sorting_state().override_force_cpu;
	stats["sort_override_force_algorithm"] = renderer->get_sorting_state().override_force_algorithm;
	stats["sort_override_forced_algorithm"] = renderer->get_sorting_state().override_forced_algorithm;

	if (has_gpu_sorter) {
		stats["gpu_sorter_algorithm"] = renderer->get_sorting_state().gpu_sorter->get_algorithm_name();
		stats["gpu_sorter_ready"] = renderer->get_sorting_state().gpu_sorter->is_ready();
		stats["gpu_sorter_max_elements"] = (int)renderer->get_sorting_state().gpu_sorter->get_max_elements();
		stats["gpu_sorter_last_sort_ms"] = renderer->get_sorting_state().gpu_sorter->get_last_sort_time_ms();

		SortingMetrics sorter_metrics = renderer->get_sorting_state().gpu_sorter->get_metrics();
		stats["gpu_sorter_avg_sort_ms"] = sorter_metrics.avg_sort_time_ms;
		stats["gpu_sorter_peak_sort_ms"] = sorter_metrics.peak_sort_time_ms;
		stats["gpu_sorter_total_sorts"] = (int64_t)sorter_metrics.total_sorts;
		stats["gpu_sorter_async_sorts"] = (int64_t)sorter_metrics.async_sorts;
		stats["gpu_sorter_total_elements"] = (int64_t)sorter_metrics.total_elements_sorted;
		stats["gpu_sorter_bandwidth_utilization"] = sorter_metrics.bandwidth_utilization;
		stats["gpu_sorter_fallback_events"] = (int64_t)sorter_metrics.fallback_events;
		stats["gpu_sorter_last_fallback_reason"] = sorter_metrics.last_fallback_reason;
		stats["gpu_sorter_fallback_reason_counts"] = sorter_metrics.fallback_reason_counts;
	} else {
		stats["gpu_sorter_algorithm"] = String();
		stats["gpu_sorter_ready"] = false;
		stats["gpu_sorter_max_elements"] = 0;
		stats["gpu_sorter_last_sort_ms"] = 0.0f;
		stats["gpu_sorter_avg_sort_ms"] = 0.0f;
		stats["gpu_sorter_peak_sort_ms"] = 0.0f;
		stats["gpu_sorter_total_sorts"] = 0;
		stats["gpu_sorter_async_sorts"] = 0;
		stats["gpu_sorter_total_elements"] = 0;
		stats["gpu_sorter_bandwidth_utilization"] = 0.0f;
		stats["gpu_sorter_fallback_events"] = 0;
		stats["gpu_sorter_last_fallback_reason"] = String();
		stats["gpu_sorter_fallback_reason_counts"] = Dictionary();
	}

	// GPU buffer manager stats
	if (renderer->get_resource_state().buffer_manager.is_valid() && renderer->get_resource_state().buffer_manager_initialized) {
		stats["buffer_manager_memory_mb"] = renderer->get_resource_state().buffer_manager->get_memory_usage_mb();
		stats["buffer_manager_capacity"] = renderer->get_resource_state().buffer_manager->get_buffer_capacity();
		stats["buffer_manager_count"] = renderer->get_resource_state().buffer_manager->get_gaussian_count();
	} else {
		stats["visible_ratio"] = 0.0;
		stats["culled_ratio"] = 0.0;
	}

	// Debug state flags
	stats["debug_show_tile_grid"] = renderer->get_debug_state().show_tile_grid;
	stats["debug_show_density_heatmap"] = renderer->get_debug_state().show_density_heatmap;
	stats["debug_show_performance_hud"] = renderer->get_debug_state().show_performance_hud;
	stats["debug_show_residency_hud"] = renderer->get_debug_state().show_residency_hud;

	// Route UIDs (overwrite snapshot values with normalized versions)
	const String normalized_route_uid = _normalize_route_uid_for_stats(renderer->get_debug_state().route_uid);
	const String normalized_sort_route_uid = _normalize_sort_route_uid_for_stats(renderer->get_debug_state().sort_route_uid);
	stats["route_uid"] = normalized_route_uid;
	stats["sort_route_uid"] = normalized_sort_route_uid;
	stats["route_uid_missing"] = RenderRouteUID::is_route_uid_missing(normalized_route_uid);
	stats["sort_route_uid_missing"] = RenderRouteUID::is_sort_route_uid_missing(normalized_sort_route_uid);

	// Instance pipeline & caching
	stats["instance_pipeline_content_generation"] =
			static_cast<int64_t>(renderer->get_resource_state().instance_pipeline_content_generation);
	stats["cached_render_reuse_enabled"] = renderer->is_cached_render_reuse_enabled();

	// GPU culler stats
	if (renderer->get_subsystem_state().gpu_culler.is_valid()) {
		const auto &cull_state = renderer->get_subsystem_state().gpu_culler->get_state();
		stats["cull_static_chunk_total"] = static_cast<int64_t>(cull_state.static_chunks.size());
		stats["cull_visible_static_chunks"] = static_cast<int64_t>(cull_state.visible_static_chunk_indices.size());
		stats["cull_gpu_visible_count"] = static_cast<int64_t>(cull_state.gpu_visible_indices_count);
		stats["cull_cpu_visible_count"] = static_cast<int64_t>(cull_state.culled_indices.size());
		stats["cull_total_splats_pre_cull"] = static_cast<int64_t>(cull_state.total_splats_pre_cull);
	} else {
		stats["cull_static_chunk_total"] = static_cast<int64_t>(0);
		stats["cull_visible_static_chunks"] = static_cast<int64_t>(0);
		stats["cull_gpu_visible_count"] = static_cast<int64_t>(0);
		stats["cull_cpu_visible_count"] = static_cast<int64_t>(0);
		stats["cull_total_splats_pre_cull"] = static_cast<int64_t>(0);
	}

	// Debug overlay metadata
	stats["debug_overlay_version"] = renderer->get_debug_state().overlay_version;
	stats["debug_tile_density_peak"] = (int)renderer->get_debug_state().tile_density_peak;
	stats["debug_tile_density_average"] = renderer->get_debug_state().tile_density_average;
	stats["debug_tile_density_size"] =
			Vector2i(renderer->get_debug_state().tile_density_width, renderer->get_debug_state().tile_density_height);

	stats["debug_preview_mode"] = renderer->get_debug_state().preview_mode;

	// Diagnostics state dictionaries
	stats["telemetry"] = diagnostics_state.last_telemetry_snapshot;
	stats["production_metrics"] = diagnostics_state.last_production_metrics;
	stats["production_metrics_validation"] = diagnostics_state.last_production_metrics_validation;
	stats["production_metrics_invalid_count"] = static_cast<int64_t>(diagnostics_state.production_metrics_invalid_count);
	stats["perf_gate"] = diagnostics_state.last_perf_gate_result;

	return stats;
}

float RenderDiagnosticsOrchestrator::get_sort_time_ms_internal() const {
	const bool sm_valid = renderer->get_debug_state().last_stage_metrics_valid;
	return sm_valid ? renderer->get_debug_state().last_stage_metrics.sort.sort_time_ms : 0.0f;
}

float RenderDiagnosticsOrchestrator::get_render_time_ms_internal() const {
	const bool sm_valid = renderer->get_debug_state().last_stage_metrics_valid;
	return sm_valid ? renderer->get_debug_state().last_stage_metrics.raster.render_time_ms : 0.0f;
}

Dictionary RenderDiagnosticsOrchestrator::get_last_sort_metrics_internal() const {
	Dictionary metrics;
	SortingStrategyConfig config = SortingStrategyConfig::load_from_project_settings();
	if (renderer->get_performance_state().last_sort_metrics_valid) {
		const GaussianSplatRenderer::SortFrameMetrics &sample = renderer->get_performance_state().last_sort_metrics;
		metrics["frame"] = sample.frame_index;
		metrics["elements"] = sample.element_count;
		metrics["total_ms"] = sample.total_ms;
		metrics["gpu_ms"] = sample.gpu_ms;
		metrics["cpu_ms"] = sample.cpu_ms;
		metrics["cpu_selection_ms"] = sample.cpu_selection_ms;
		metrics["algorithm"] = String(sample.algorithm);
		metrics["used_gpu"] = sample.used_gpu;
		metrics["cpu_fallback"] = sample.used_cpu_fallback;
		metrics["hybrid"] = sample.used_hybrid;
	}
	metrics["target_ms"] = config.target_sort_time_ms;
	const auto &sorting_state = renderer->get_sorting_state();
	metrics["active_algorithm"] = sorting_state.active_sort_algorithm;
	metrics["switch_reason"] = sorting_state.sort_switch_reason;
	metrics["override_force_cpu"] = sorting_state.override_force_cpu;
	metrics["override_force_algorithm"] = sorting_state.override_force_algorithm;
	metrics["override_forced_algorithm"] = sorting_state.override_forced_algorithm;
	return metrics;
}

Array RenderDiagnosticsOrchestrator::get_sort_metrics_history_internal() const {
	Array history;
	if (renderer->get_performance_state().last_sort_metrics_valid) {
		const GaussianSplatRenderer::SortFrameMetrics &sample = renderer->get_performance_state().last_sort_metrics;
		Dictionary entry;
		entry["frame"] = sample.frame_index;
		entry["elements"] = sample.element_count;
		entry["total_ms"] = sample.total_ms;
		entry["gpu_ms"] = sample.gpu_ms;
		entry["cpu_ms"] = sample.cpu_ms;
		entry["cpu_selection_ms"] = sample.cpu_selection_ms;
		entry["algorithm"] = String(sample.algorithm);
		entry["used_gpu"] = sample.used_gpu;
		entry["cpu_fallback"] = sample.used_cpu_fallback;
		entry["hybrid"] = sample.used_hybrid;
		history.push_back(entry);
	}
	return history;
}

void RenderDiagnosticsOrchestrator::record_sort_sample(const GaussianSplatRenderer::SortFrameMetrics &p_sample) {
	SortingStrategyConfig config = SortingStrategyConfig::load_from_project_settings();
	renderer->get_performance_state().last_sort_metrics = p_sample;
	renderer->get_performance_state().last_sort_metrics_valid = true;

	if (config.log_metrics && config.log_interval_frames > 0) {
		if (p_sample.frame_index % config.log_interval_frames == 0) {
			GS_LOG_GPU_SORT_INFO(vformat("[Sort Metrics] frame=%d elements=%d total=%.2f ms gpu=%.2f ms cpu=%.2f ms",
					p_sample.frame_index,
					p_sample.element_count,
					p_sample.total_ms,
					p_sample.gpu_ms,
					p_sample.cpu_ms));
		}
	}
}

void RenderDiagnosticsOrchestrator::finalize_frame_metrics(uint64_t p_frame_start_usec) {
	debug_state_orchestrator->update_frame_times(0.0f, 0.0f);

	// Update frame timing tracked in diagnostics_state
	uint64_t frame_end = OS::get_singleton()->get_ticks_usec();
	float frame_time_ms = (frame_end - p_frame_start_usec) / 1000.0f;

	// Frame-to-frame timing (measures actual throughput including GPU wait)
	if (diagnostics_state.last_frame_start_usec > 0) {
		diagnostics_state.frame_to_frame_time_ms =
				(p_frame_start_usec - diagnostics_state.last_frame_start_usec) / 1000.0f;
	}
	diagnostics_state.last_frame_start_usec = p_frame_start_usec;

	diagnostics_state.total_frames_rendered++;

	// Update rolling average frame time (use frame-to-frame for true FPS)
	float alpha = 0.1f; // Smoothing factor
	float timing_for_avg = diagnostics_state.frame_to_frame_time_ms > 0.0f
			? diagnostics_state.frame_to_frame_time_ms
			: frame_time_ms;
	if (diagnostics_state.total_frames_rendered <= 2) {
		diagnostics_state.avg_frame_time_ms = timing_for_avg;
		diagnostics_state.avg_frame_to_frame_ms = timing_for_avg;
	} else {
		diagnostics_state.avg_frame_time_ms =
				diagnostics_state.avg_frame_time_ms * (1.0f - alpha) + frame_time_ms * alpha;
		diagnostics_state.avg_frame_to_frame_ms =
				diagnostics_state.avg_frame_to_frame_ms * (1.0f - alpha) + timing_for_avg * alpha;
	}

	diagnostics_state.peak_frame_time_ms =
			MAX(diagnostics_state.peak_frame_time_ms, diagnostics_state.frame_to_frame_time_ms);

	// Ensure GPU timestamps are resolved before logging
	renderer->update_gpu_pass_metrics_from_tile_renderer();

	ProductionMetricsConfig metrics_config = _load_production_metrics_config();
	const bool stage_metrics_valid = renderer->get_debug_state().last_stage_metrics_valid;
	GaussianSplatRenderer::StageMetrics stage_metrics = stage_metrics_valid
			? renderer->get_debug_state().last_stage_metrics
			: GaussianSplatRenderer::StageMetrics();
	Dictionary production_metrics = _build_production_metrics_snapshot(
			*renderer, stage_metrics, stage_metrics_valid, frame_time_ms);
	Dictionary telemetry_snapshot = production_metrics.duplicate();
	_append_telemetry_extras(*renderer, stage_metrics, stage_metrics_valid, frame_time_ms, telemetry_snapshot);
	diagnostics_state.last_production_metrics = production_metrics;
	diagnostics_state.last_telemetry_snapshot = telemetry_snapshot;

	if (metrics_config.validate_metrics) {
		diagnostics_state.last_production_metrics_validation =
				_validate_production_metrics(diagnostics_state.last_production_metrics);
		if (!bool(diagnostics_state.last_production_metrics_validation.get("valid", true))) {
			diagnostics_state.production_metrics_invalid_count++;
		}
	} else {
		Dictionary validation;
		validation["valid"] = true;
		validation["disabled"] = true;
		validation["frame"] = diagnostics_state.last_production_metrics.get("frame", int64_t(0));
		diagnostics_state.last_production_metrics_validation = validation;
	}

	diagnostics_state.last_perf_gate_result =
			_evaluate_perf_gate(metrics_config, diagnostics_state.last_production_metrics);

	_update_production_summary(diagnostics_state, metrics_config, diagnostics_state.last_production_metrics,
			diagnostics_state.last_perf_gate_result, frame_end);

	// Populate the canonical per-frame diagnostics snapshot once all stages have
	// completed and all metrics are up to date.
	build_diagnostics_snapshot();
}

void RenderDiagnosticsOrchestrator::build_diagnostics_snapshot() {
	GaussianSplatDiagnosticsSnapshot &snap = renderer->get_diagnostics_snapshot();
	snap.clear();

	const auto &data_source_info = renderer->get_performance_state().data_source_info;
	const auto &frame_state = renderer->get_frame_state();
	const auto &debug_state = renderer->get_debug_state();
	const bool has_last_sort_metrics = renderer->get_performance_state().last_sort_metrics_valid;
	const GaussianSplatRenderer::SortFrameMetrics &last_sort_metrics = renderer->get_performance_state().last_sort_metrics;

	// Populate frame_index early so downstream sections can use it for
	// freshness checks (e.g. sort history staleness guard).
	snap.frame_index = frame_state.frame_counter;

	// ---------------------------------------------------------------
	// GPU Pipeline Timing
	// ---------------------------------------------------------------
	// Read GPU pipeline timing directly from TileRenderer when available.
	const bool has_tile_renderer = renderer->get_tile_renderer_state().renderer.is_valid();
	if (has_tile_renderer) {
		const TileRenderer *tr = renderer->get_tile_renderer_state().renderer.ptr();
		snap.pipeline_frame_time_ms = tr->get_last_gpu_frame_time_ms();
		snap.pipeline_binning_time_ms = tr->get_last_gpu_binning_time_ms();
		snap.pipeline_prefix_time_ms = tr->get_last_gpu_prefix_time_ms();
		snap.pipeline_raster_time_ms = tr->get_last_gpu_raster_time_ms();
		snap.pipeline_resolve_time_ms = tr->get_last_gpu_resolve_time_ms();
	}

	// GPU cull time from stage metrics when valid.
	const bool stage_metrics_valid = debug_state.last_stage_metrics_valid;
	const GaussianSplatRenderer::StageMetrics &stage_metrics = debug_state.last_stage_metrics;
	if (stage_metrics_valid) {
		snap.pipeline_cull_time_ms = stage_metrics.cull.cull_time_ms;
	}

	// GPU sort time: from the latest sort metrics sample, only when GPU sort was used.
	// Guard: only use sort data if it belongs to the current frame; stale samples from
	// skip/reuse frames would misattribute previous-frame timing to this frame.
	if (has_last_sort_metrics && last_sort_metrics.frame_index == snap.frame_index && last_sort_metrics.used_gpu) {
		snap.pipeline_sort_time_ms = last_sort_metrics.gpu_ms;
	}

	// GPU composite time: from stage metrics when composite actually executed.
	if (stage_metrics_valid && stage_metrics.composite_executed) {
		snap.pipeline_composite_time_ms = stage_metrics.composite_time_ms;
	}

	// ---------------------------------------------------------------
	// CPU Timing
	// ---------------------------------------------------------------
	if (has_tile_renderer) {
		snap.cpu_setup_time_ms = renderer->get_tile_renderer_state().renderer->get_last_setup_cpu_ms();
	}
	// CPU sort timing was previously cached in PerformanceMetrics; those fields
	// have been removed.  The snapshot retains 0.0f defaults.

	// ---------------------------------------------------------------
	// Sort Metadata
	// ---------------------------------------------------------------
	// Only populate from sort history if the entry matches the current frame.
	// On skip/reuse frames the history still holds previous-frame data which
	// must not be stamped onto this frame's snapshot.
	if (has_last_sort_metrics && last_sort_metrics.frame_index == snap.frame_index) {
		snap.sort_used_gpu = last_sort_metrics.used_gpu;
		snap.sort_used_cpu_fallback = last_sort_metrics.used_cpu_fallback;
		snap.sort_algorithm = last_sort_metrics.algorithm;
		snap.sort_element_count = last_sort_metrics.element_count;
	}

	// ---------------------------------------------------------------
	// Visibility / Projection Stats
	// ---------------------------------------------------------------
	snap.visible_splat_count = frame_state.visible_splat_count.load(std::memory_order_acquire);
	if (has_tile_renderer) {
		const TileRenderer *tr = renderer->get_tile_renderer_state().renderer.ptr();
		snap.total_processed = tr->get_total_processed();
		snap.projection_success_count = tr->get_projection_success_count();
		snap.projection_success_rate_pct = tr->get_projection_success_rate_pct();
		snap.clip_reject_count = tr->get_clip_bounds_reject_count();
		snap.radius_reject_count = tr->get_radius_reject_count();
		snap.viewport_reject_count = tr->get_viewport_bounds_reject_count();
		snap.extreme_aspect_count = tr->get_extreme_aspect_count();
		snap.index_mismatch_count = tr->get_index_mismatch_count();
	}

	// ---------------------------------------------------------------
	// Tile Stats
	// ---------------------------------------------------------------
	if (has_tile_renderer) {
		const TileRenderer *tr = renderer->get_tile_renderer_state().renderer.ptr();
		snap.tile_count = tr->get_tile_count();
		snap.overflow_tile_count = tr->get_overflow_tile_count();
		snap.clamped_records = tr->get_clamped_records();
		snap.aggregated_count = tr->get_aggregated_count();

		const TileRenderer::RenderStats tile_stats = tr->get_last_render_stats();
		snap.overlap_records_used = tile_stats.overlap_records;
		snap.overlap_record_budget = tile_stats.overlap_record_budget;

		// SH Cache
		snap.sh_cache_hits = tr->get_sh_cache_hits();
		snap.sh_cache_updates = tr->get_sh_cache_updates();
		snap.sh_cache_hit_rate_pct = tr->get_sh_cache_hit_rate_pct();
	}

	// ---------------------------------------------------------------
	// Stage Metrics
	// ---------------------------------------------------------------
	snap.stage_metrics_valid = stage_metrics_valid;
	if (stage_metrics_valid) {
		// Cull stage
		snap.stage_cull_candidate_count = stage_metrics.cull.candidate_count;
		snap.stage_cull_visible_count = stage_metrics.cull.visible_count;

		// Sort stage
		snap.stage_sort_did_sort = stage_metrics.sort.did_sort;
		snap.stage_sort_input_count = stage_metrics.sort.input_count;
		snap.stage_sort_sorted_count = stage_metrics.sort.sorted_count;

		// Raster stage
		snap.stage_raster_reused_cached = stage_metrics.raster.reused_cached_render;
		snap.stage_raster_painterly_active = stage_metrics.raster.painterly_active;

		// Composite stage
		snap.stage_composite_executed = stage_metrics.composite_executed;
	}

	// ---------------------------------------------------------------
	// Frame Metadata
	// ---------------------------------------------------------------
	// Note: snap.frame_index is populated early (before GPU pipeline timing)
	// so that sort history freshness guards can compare against it.
	snap.frame_time_ms = diagnostics_state.frame_to_frame_time_ms;
	snap.telemetry_active = !diagnostics_state.last_telemetry_snapshot.is_empty();
	snap.route_uid = debug_state.route_uid;
	snap.sort_route_uid = debug_state.sort_route_uid;
	snap.data_source = data_source_info.data_source;

	// ---------------------------------------------------------------
	// Streaming / VRAM / LOD / Memory Stream / Compression
	// ---------------------------------------------------------------
	const auto &streaming_state = renderer->get_streaming_state();
	if (streaming_state.current_streaming_system.is_valid()) {
		GaussianSplatDiagnosticsSnapshot::StreamingSnapshot &stream = snap.streaming;
		stream.clear();

		const Dictionary analytics = streaming_state.current_streaming_system->get_streaming_analytics();
		const Dictionary vram_stats = streaming_state.current_streaming_system->get_vram_debug_stats();
		const Dictionary cull_stats = streaming_state.current_streaming_system->get_chunk_culling_stats();
		const Dictionary lod_stats = streaming_state.current_streaming_system->get_lod_debug_stats();

		// VRAM budget regulation.
		stream.vram_current_usage_mb = _dict_get_f32(vram_stats, "current_usage_bytes") / (1024.0f * 1024.0f);
		stream.vram_budget_mb = _dict_get_f32(vram_stats, "budget_bytes") / (1024.0f * 1024.0f);
		stream.vram_usage_percent = _dict_get_f32(vram_stats, "usage_percent");
		stream.vram_current_max_chunks = static_cast<uint32_t>(_dict_get_i64(vram_stats, "current_max_chunks"));
		stream.vram_loaded_chunks = static_cast<uint32_t>(_dict_get_i64(vram_stats, "loaded_chunks"));
		stream.vram_evicted_this_frame = static_cast<uint32_t>(_dict_get_i64(vram_stats, "evicted_this_frame"));
		stream.vram_loaded_this_frame = static_cast<uint32_t>(_dict_get_i64(vram_stats, "loaded_this_frame"));
		stream.vram_budget_warning_active = _dict_get_bool(vram_stats, "budget_warning_active");
		stream.vram_regulation_adjustments = static_cast<uint32_t>(_dict_get_i64(vram_stats, "regulation_adjustments"));
		stream.vram_thrashing_events = static_cast<uint32_t>(_dict_get_i64(vram_stats, "thrashing_events"));

		// Streaming core.
		stream.streaming_monitor_ready = streaming_state.current_streaming_system->is_runtime_ready();
		stream.streaming_runtime_capacity_zero = _dict_get_bool(analytics, "runtime_capacity_zero");
		stream.streaming_runtime_buffer_invalid = _dict_get_bool(analytics, "runtime_buffer_invalid");
		stream.streaming_invalid_camera_inputs = static_cast<uint32_t>(_dict_get_i64(cull_stats, "invalid_camera_input_events"));
		stream.streaming_total_chunks = static_cast<uint32_t>(_dict_get_i64(cull_stats, "total_chunks"));
		stream.streaming_visible_chunks = static_cast<uint32_t>(_dict_get_i64(cull_stats, "visible_chunks"));
		stream.streaming_loaded_chunks = static_cast<uint32_t>(_dict_get_i64(cull_stats, "loaded_chunks"));
		stream.streaming_frustum_culled_chunks = static_cast<uint32_t>(_dict_get_i64(cull_stats, "frustum_culled_chunks"));
		stream.streaming_vram_usage_mb = static_cast<float>(streaming_state.current_streaming_system->get_vram_usage()) / (1024.0f * 1024.0f);
		stream.streaming_chunks_loaded_this_frame = streaming_state.current_streaming_system->get_chunks_loaded_this_frame();
		stream.streaming_chunks_evicted_this_frame = streaming_state.current_streaming_system->get_chunks_evicted_this_frame();
		stream.streaming_visible_count = streaming_state.current_streaming_system->get_visible_count();
		stream.streaming_buffer_capacity_splats = streaming_state.current_streaming_system->get_buffer_capacity_splats();
		stream.streaming_effective_splat_count = streaming_state.current_streaming_system->get_effective_splat_count();
		stream.streaming_visible_change_ratio = streaming_state.current_streaming_system->get_visible_chunk_change_ratio();
		stream.streaming_lod_blend_factor = streaming_state.current_streaming_system->get_global_lod_blend_factor();
		stream.streaming_sh_band_level = streaming_state.current_streaming_system->get_global_sh_band_level();
		stream.streaming_effective_upload_cap_mb_per_frame = _dict_get_f32(analytics, "effective_upload_cap_mb_per_frame");
		stream.streaming_effective_upload_cap_mb_per_slice = _dict_get_f32(analytics, "effective_upload_cap_mb_per_slice");
		stream.streaming_effective_upload_cap_mb_per_second = _dict_get_f32(analytics, "effective_upload_cap_mb_per_second");
		stream.streaming_effective_vram_budget_mb = _dict_get_f32(analytics, "effective_vram_budget_mb");
		stream.streaming_effective_vram_max_chunks = static_cast<uint32_t>(_dict_get_i64(analytics, "effective_vram_max_chunks"));
		stream.streaming_upload_frame_cap_hit = _dict_get_bool(analytics, "upload_frame_cap_hit");
		stream.streaming_upload_bandwidth_cap_hit = _dict_get_bool(analytics, "upload_bandwidth_cap_hit");
		stream.streaming_chunk_load_cap_hit = _dict_get_bool(analytics, "chunk_load_cap_hit");
		stream.streaming_vram_chunk_cap_hit = _dict_get_bool(analytics, "vram_chunk_cap_hit");
		stream.streaming_queue_pressure_active = _dict_get_bool(analytics, "queue_pressure_active");

		// LOD system.
		stream.lod_current_level = static_cast<int32_t>(_dict_get_i64(lod_stats, "current_lod_level"));
		stream.lod_target_distance = _dict_get_f32(lod_stats, "lod_target_distance");
		stream.lod_hysteresis_zone = streaming_state.current_streaming_system->get_lod_hysteresis_zone();
		stream.lod_blend_distance = streaming_state.current_streaming_system->get_lod_blend_distance();
		stream.lod_transitions_this_frame = static_cast<uint32_t>(_dict_get_i64(lod_stats, "transitions_this_frame"));
		stream.lod_splat_skip_factor = static_cast<uint32_t>(_dict_get_i64(lod_stats, "max_splat_skip_factor", 1));
		stream.lod_opacity_multiplier = _dict_get_f32(lod_stats, "min_opacity_multiplier", 1.0f);
		stream.lod_effective_count_after_skip = streaming_state.current_streaming_system->get_effective_splat_count();
		stream.lod_chunk_blend_factors_avg = streaming_state.current_streaming_system->get_global_lod_blend_factor();
		stream.lod_chunks_in_transition = static_cast<uint32_t>(_dict_get_i64(lod_stats, "chunks_in_transition"));
		stream.lod_min_chunk_distance = _dict_get_f32(lod_stats, "min_distance");
		stream.lod_max_chunk_distance = _dict_get_f32(lod_stats, "max_distance");
		stream.lod_avg_chunk_distance = _dict_get_f32(lod_stats, "avg_distance");
		stream.lod_reduction_ratio_pct = 100.0f * _dict_get_f32(lod_stats, "reduction_ratio");

		if (lod_stats.has("lod_distribution")) {
			const Array lod_dist = lod_stats["lod_distribution"];
			stream.lod_level_0_chunk_count = lod_dist.size() > 0 ? static_cast<uint32_t>(static_cast<int64_t>(lod_dist[0])) : 0u;
		}
		if (lod_stats.has("sh_band_distribution")) {
			const Array sh_dist = lod_stats["sh_band_distribution"];
			stream.lod_sh_band_3_chunk_count = sh_dist.size() > 3 ? static_cast<uint32_t>(static_cast<int64_t>(sh_dist[3])) : 0u;
		}

		Ref<VRAMBudgetRegulator> regulator = streaming_state.current_streaming_system->get_vram_regulator();
		if (regulator.is_valid()) {
			stream.lod_distance_multiplier = regulator->get_lod_distance_multiplier();
			stream.lod_quality_degradation_active = stream.lod_distance_multiplier > 1.0f;
		}

		// Memory stream.
		if (streaming_state.memory_stream.is_valid()) {
			const StreamingStats ms_stats = streaming_state.memory_stream->get_stats();
			stream.memory_stream_total_bytes_uploaded_mb = static_cast<float>(ms_stats.total_bytes_uploaded) / (1024.0f * 1024.0f);
			stream.memory_stream_total_bytes_downloaded_mb = static_cast<float>(ms_stats.total_bytes_downloaded) / (1024.0f * 1024.0f);
			stream.memory_stream_buffer_switches = ms_stats.buffer_switches;
			stream.memory_stream_stalls = ms_stats.stalls;
			stream.memory_stream_stall_percent = ms_stats.total_frames == 0
					? 0.0f
					: 100.0f * static_cast<float>(ms_stats.stalls) / static_cast<float>(ms_stats.total_frames);
			stream.memory_stream_pool_hits = ms_stats.pool_hits;
			stream.memory_stream_pool_misses = ms_stats.pool_misses;
			const uint32_t pool_total = ms_stats.pool_hits + ms_stats.pool_misses;
			stream.memory_stream_pool_hit_rate_pct = pool_total == 0
					? 0.0f
					: 100.0f * static_cast<float>(ms_stats.pool_hits) / static_cast<float>(pool_total);
			stream.memory_stream_peak_memory_mb = ms_stats.peak_memory_mb;
			stream.memory_stream_defrag_count = ms_stats.defrag_count;

			// Keep existing streaming monitor behavior.
			stream.streaming_bytes_uploaded_mb = static_cast<float>(ms_stats.total_bytes_uploaded) / (1024.0f * 1024.0f);
			stream.streaming_buffer_switches = ms_stats.buffer_switches;
		} else {
			stream.memory_stream_total_bytes_downloaded_mb = _dict_get_f32(analytics, "evicted_bytes_total_mb");
		}

		// Chunk management + pack/upload timing.
		const uint32_t prefetch_hits = static_cast<uint32_t>(_dict_get_i64(analytics, "prefetch_hits",
				_dict_get_i64(analytics, "scheduler_prefetch_candidates")));
		const uint32_t prefetch_misses = static_cast<uint32_t>(_dict_get_i64(analytics, "prefetch_misses"));
		stream.chunk_prefetch_hits = prefetch_hits;
		stream.chunk_prefetch_misses = prefetch_misses;
		const uint32_t prefetch_total = prefetch_hits + prefetch_misses;
		stream.chunk_prefetch_efficiency_pct = prefetch_total == 0
				? 0.0f
				: 100.0f * static_cast<float>(prefetch_hits) / static_cast<float>(prefetch_total);
		stream.chunk_camera_velocity = _dict_get_f32(analytics, "camera_velocity");
		stream.chunk_average_load_time_ms = _dict_get_f32(analytics, "avg_chunk_load_time_ms",
				_dict_get_f32(analytics, "pack_avg_ms"));
		stream.chunk_upload_queue_depth = static_cast<uint32_t>(_dict_get_i64(analytics, "pending_uploads",
				_dict_get_i64(analytics, "scheduler_upload_queue_depth")));
		stream.chunk_pack_jobs_in_flight = static_cast<uint32_t>(_dict_get_i64(analytics, "pack_jobs_in_flight"));
		stream.chunk_total_capacity_mb = (static_cast<float>(streaming_state.current_streaming_system->get_buffer_capacity_splats()) * sizeof(PackedGaussian)) / (1024.0f * 1024.0f);

		stream.pack_avg_time_ms = _dict_get_f32(analytics, "pack_avg_ms");
		stream.pack_max_time_ms = _dict_get_f32(analytics, "pack_max_ms");
		stream.pack_jobs_completed = static_cast<uint32_t>(_dict_get_i64(analytics, "pack_jobs_completed"));
		stream.upload_mb_this_frame = _dict_get_f32(analytics, "upload_mb_this_frame");
		stream.upload_chunks_this_frame = static_cast<uint32_t>(_dict_get_i64(analytics, "upload_chunks_this_frame"));

		// SH compression analytics.
		const SHCompressionMetrics sh_metrics = streaming_state.current_streaming_system->get_total_sh_metrics();
		stream.sh_compression_raw_mb = static_cast<float>(sh_metrics.raw_bytes) / (1024.0f * 1024.0f);
		stream.sh_compression_compressed_mb = static_cast<float>(sh_metrics.compressed_bytes) / (1024.0f * 1024.0f);
		stream.sh_compression_ratio_pct = sh_metrics.raw_bytes > 0
				? 100.0f * static_cast<float>(sh_metrics.compressed_bytes) / static_cast<float>(sh_metrics.raw_bytes)
				: 0.0f;

		snap.streaming_valid = true;
	}

	// ---------------------------------------------------------------
	// Mark valid
	// ---------------------------------------------------------------
	snap.valid = true;
}

Dictionary RenderDiagnosticsOrchestrator::get_runtime_diagnostic_snapshot() const {
	Dictionary snapshot;
	snapshot["frame"] = static_cast<int64_t>(renderer->get_frame_state().frame_counter);
	snapshot["device_capability_report"] = build_device_capability_report();
	snapshot["texture_allocation_trace"] = serialize_texture_trace();
	snapshot["cross_device_operation_log"] = serialize_cross_device_operations();
	snapshot["frame_timing_analysis"] = serialize_frame_timing();
	snapshot["error_statistics"] = serialize_error_statistics();
	snapshot["production_metrics_contract"] = _production_metrics_contract();
	snapshot["production_metrics"] = diagnostics_state.last_production_metrics;
	snapshot["production_metrics_validation"] = diagnostics_state.last_production_metrics_validation;
	snapshot["production_metrics_invalid_count"] = static_cast<int64_t>(diagnostics_state.production_metrics_invalid_count);
	snapshot["perf_gate"] = diagnostics_state.last_perf_gate_result;
	snapshot["telemetry"] = diagnostics_state.last_telemetry_snapshot;

	Array summaries;
	for (const Dictionary &summary : diagnostics_state.production_metrics_summaries) {
		summaries.push_back(summary);
	}
	snapshot["production_metrics_summaries"] = summaries;

	// Read debug modes from DebugOverlaySystem (Phase 8 migration)
	Dictionary debug_modes;
	if (renderer->get_subsystem_state().debug_overlay_system.is_valid()) {
		debug_modes["tile_bounds"] = renderer->get_subsystem_state().debug_overlay_system->get_show_tile_bounds();
		debug_modes["splat_coverage"] = renderer->get_subsystem_state().debug_overlay_system->get_show_splat_coverage();
		debug_modes["overflow_tiles"] = renderer->get_subsystem_state().debug_overlay_system->get_show_overflow_tiles();
		debug_modes["projection_issues"] = renderer->get_subsystem_state().debug_overlay_system->get_show_projection_issues();
		debug_modes["tile_grid"] = renderer->get_subsystem_state().debug_overlay_system->get_show_tile_grid();
		debug_modes["density_heatmap"] = renderer->get_subsystem_state().debug_overlay_system->get_show_density_heatmap();
		debug_modes["performance_hud"] = renderer->get_subsystem_state().debug_overlay_system->get_show_performance_hud();
		debug_modes["residency_hud"] = renderer->get_subsystem_state().debug_overlay_system->get_show_residency_hud();
		debug_modes["device_boundaries"] = renderer->get_subsystem_state().debug_overlay_system->get_show_device_boundaries();
		debug_modes["texture_states"] = renderer->get_subsystem_state().debug_overlay_system->get_show_texture_states();
		debug_modes["resolve_input"] = renderer->get_subsystem_state().debug_overlay_system->get_show_resolve_input();
		debug_modes["resolve_output"] = renderer->get_subsystem_state().debug_overlay_system->get_show_resolve_output();
	}
	snapshot["debug_modes"] = debug_modes;

	Dictionary gpu_performance;
	const bool has_tr = renderer->get_tile_renderer_state().renderer.is_valid();
	if (has_tr) {
		const TileRenderer *tr = renderer->get_tile_renderer_state().renderer.ptr();
		gpu_performance["frame_gpu_ms"] = tr->get_last_gpu_frame_time_ms();
		gpu_performance["binning_gpu_ms"] = tr->get_last_gpu_binning_time_ms();
		gpu_performance["raster_gpu_ms"] = tr->get_last_gpu_raster_time_ms();
		gpu_performance["prefix_gpu_ms"] = tr->get_last_gpu_prefix_time_ms();
		gpu_performance["resolve_gpu_ms"] = tr->get_last_gpu_resolve_time_ms();
		if (renderer->get_subsystem_state().rasterizer.is_valid()) {
			RasterPerformance rp = renderer->get_subsystem_state().rasterizer->get_performance();
			gpu_performance["timing_frame_serial"] = (int64_t)rp.timing_frame_serial;
			gpu_performance["timing_frames_behind"] = (int64_t)rp.timing_frames_behind;
		}
		GPUPerformanceMonitor::SummaryMetrics timeline =
				renderer->get_tile_renderer_state().gpu_performance_monitor.get_summary_metrics();
		gpu_performance["timeline_inflight_frames"] = (int64_t)timeline.inflight_frames;
		gpu_performance["timeline_completed_frames"] = (int64_t)timeline.completed_frames;
		gpu_performance["timeline_stall_count"] = (int64_t)timeline.stall_count;
		gpu_performance["timeline_stall_ms"] = float(timeline.total_stall_ns) / 1000000.0f;
		gpu_performance["timeline_last_value"] = (int64_t)timeline.last_frame_index;
		gpu_performance["utilization_percent"] = renderer->get_tile_renderer_state().gpu_performance_monitor.get_gpu_utilization_async() * 100.0f;
	} else {
		gpu_performance["utilization_percent"] = 0.0f;
		gpu_performance["frame_gpu_ms"] = 0.0f;
		gpu_performance["binning_gpu_ms"] = 0.0f;
		gpu_performance["raster_gpu_ms"] = 0.0f;
		gpu_performance["prefix_gpu_ms"] = 0.0f;
		gpu_performance["resolve_gpu_ms"] = 0.0f;
	}
	snapshot["gpu_performance"] = gpu_performance;

	return snapshot;
}

Dictionary GaussianSplatRenderer::get_render_stats() const {
	ERR_FAIL_NULL_V(diagnostics_orchestrator, Dictionary());
	return diagnostics_orchestrator->build_render_stats();
}

float GaussianSplatRenderer::get_sort_time_ms() const {
	ERR_FAIL_NULL_V(diagnostics_orchestrator, 0.0f);
	return diagnostics_orchestrator->get_sort_time_ms_internal();
}

float GaussianSplatRenderer::get_render_time_ms() const {
	ERR_FAIL_NULL_V(diagnostics_orchestrator, 0.0f);
	return diagnostics_orchestrator->get_render_time_ms_internal();
}

Dictionary GaussianSplatRenderer::get_last_sort_metrics() const {
	ERR_FAIL_NULL_V(diagnostics_orchestrator, Dictionary());
	return diagnostics_orchestrator->get_last_sort_metrics_internal();
}

Array GaussianSplatRenderer::get_sort_metrics_history() const {
	ERR_FAIL_NULL_V(diagnostics_orchestrator, Array());
	return diagnostics_orchestrator->get_sort_metrics_history_internal();
}

void GaussianSplatRenderer::record_sort_sample(const SortFrameMetrics &p_sample) {
	ERR_FAIL_NULL(diagnostics_orchestrator);
	diagnostics_orchestrator->record_sort_sample(p_sample);
}

Dictionary GaussianSplatRenderer::get_runtime_diagnostic_snapshot() const {
	return diagnostics_orchestrator->get_runtime_diagnostic_snapshot();
}

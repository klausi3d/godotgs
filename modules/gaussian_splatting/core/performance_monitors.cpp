#include "performance_monitors.h"
#include "../renderer/gaussian_splat_renderer.h"
#include "../renderer/gaussian_gpu_layout.h"
#include "../renderer/render_types/diagnostics_snapshot.h"
#include "core/math/math_funcs.h"
#include "main/performance.h"

GaussianSplattingPerformanceMonitors *GaussianSplattingPerformanceMonitors::singleton = nullptr;

static float _sanitize_ms(float p_value) {
    if (p_value < 0.0f) {
        return 0.0f;
    }
    if (Math::is_nan(p_value) || Math::is_inf(p_value)) {
        return 0.0f;
    }
    return p_value;
}

static const GaussianSplatDiagnosticsSnapshot *_get_valid_snapshot(const GaussianSplatRenderer *p_renderer) {
	if (!p_renderer) {
		return nullptr;
	}
	const GaussianSplatDiagnosticsSnapshot &snapshot = p_renderer->get_diagnostics_snapshot();
	if (!snapshot.valid) {
		return nullptr;
	}
	return &snapshot;
}

static const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *_get_valid_streaming_snapshot(const GaussianSplatRenderer *p_renderer) {
	const GaussianSplatDiagnosticsSnapshot *snapshot = _get_valid_snapshot(p_renderer);
	if (!snapshot || !snapshot->streaming_valid) {
		return nullptr;
	}
	return &snapshot->streaming;
}

GaussianSplattingPerformanceMonitors::GaussianSplattingPerformanceMonitors() {
}

GaussianSplattingPerformanceMonitors::~GaussianSplattingPerformanceMonitors() {
    cleanup_monitors();
}

GaussianSplattingPerformanceMonitors *GaussianSplattingPerformanceMonitors::get_singleton() {
    return singleton;
}

void GaussianSplattingPerformanceMonitors::create_singleton() {
    if (!singleton) {
        singleton = memnew(GaussianSplattingPerformanceMonitors);
        singleton->initialize_monitors();
    }
}

void GaussianSplattingPerformanceMonitors::destroy_singleton() {
    if (singleton) {
        memdelete(singleton);
        singleton = nullptr;
    }
}

void GaussianSplattingPerformanceMonitors::register_splat_renderer(GaussianSplatRenderer *p_renderer) {
    ERR_FAIL_NULL(p_renderer);
    if (!monitors_registered) {
        initialize_monitors();
    }

    // Add to tracking list if not already registered
    if (!registered_splat_renderers.has(p_renderer)) {
        registered_splat_renderers.push_back(p_renderer);
    }

    // Always prefer the most recently registered renderer so monitor values
    // follow the currently active viewport/session.
    active_splat_renderer = p_renderer;
}

void GaussianSplattingPerformanceMonitors::unregister_splat_renderer(GaussianSplatRenderer *p_renderer) {
    // Remove from tracking list
    registered_splat_renderers.erase(p_renderer);

    // If this was the active renderer, switch to another if available
    if (active_splat_renderer == p_renderer) {
        active_splat_renderer = registered_splat_renderers.size() > 0 ? registered_splat_renderers[0] : nullptr;
    }
}

GaussianSplatRenderer *GaussianSplattingPerformanceMonitors::_get_active_splat_renderer(bool p_require_streaming) const {
    (void)p_require_streaming;
    if (active_splat_renderer) {
        return active_splat_renderer;
    }
    return registered_splat_renderers.size() > 0 ? registered_splat_renderers[0] : nullptr;
}

void GaussianSplattingPerformanceMonitors::_register_monitor_definitions(Performance *p_perf) {
    struct MonitorDefinition {
        const char *name;
        Callable callable;
    };

    const MonitorDefinition monitor_definitions[] = {
        // Pipeline Timing Monitors
        { "gaussian_splatting/pipeline_frame_time_ms",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_pipeline_frame_time_ms) },
        { "gaussian_splatting/pipeline_cull_time_ms",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_pipeline_cull_time_ms) },
        { "gaussian_splatting/pipeline_sort_time_ms",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_pipeline_sort_time_ms) },
        { "gaussian_splatting/pipeline_binning_time_ms",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_pipeline_binning_time_ms) },
        { "gaussian_splatting/pipeline_prefix_time_ms",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_pipeline_prefix_time_ms) },
        { "gaussian_splatting/pipeline_raster_time_ms",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_pipeline_raster_time_ms) },
        { "gaussian_splatting/pipeline_resolve_time_ms",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_pipeline_resolve_time_ms) },
        { "gaussian_splatting/pipeline_composite_time_ms",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_pipeline_composite_time_ms) },

        // CPU Timing Monitors
        { "gaussian_splatting/telemetry_active",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_telemetry_active) },
        { "gaussian_splatting/cpu_setup_time_ms",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_cpu_setup_time_ms) },
        { "gaussian_splatting/cpu_sort_submit_ms",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_cpu_sort_submit_ms) },
        { "gaussian_splatting/cpu_sort_wait_ms",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_cpu_sort_wait_ms) },
        { "gaussian_splatting/cpu_sort_input_build_ms",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_cpu_sort_input_build_ms) },

        // Projection Statistics Monitors
        { "gaussian_splatting/visible_splats",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_visible_splat_count) },
        { "gaussian_splatting/total_processed",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_total_processed) },
        { "gaussian_splatting/projection_success_count",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_projection_success_count) },
        { "gaussian_splatting/projection_success_rate_pct",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_projection_success_rate_pct) },

        // Rejection Statistics Monitors
        { "gaussian_splatting/clip_reject_count",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_clip_reject_count) },
        { "gaussian_splatting/radius_reject_count",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_radius_reject_count) },
        { "gaussian_splatting/viewport_reject_count",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_viewport_reject_count) },

        // Quality Monitors
        { "gaussian_splatting/extreme_aspect_count",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_extreme_aspect_count) },
        { "gaussian_splatting/index_mismatch_count",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_index_mismatch_count) },

        // SH Cache Monitors
        { "gaussian_splatting/sh_cache_hits",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_sh_cache_hits) },
        { "gaussian_splatting/sh_cache_updates",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_sh_cache_updates) },
        { "gaussian_splatting/sh_cache_hit_rate_pct",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_sh_cache_hit_rate_pct) },

        // Overflow Statistics Monitors
        { "gaussian_splatting/overflow_tile_count",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_overflow_tile_count) },
        { "gaussian_splatting/clamped_records",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_clamped_records) },
        { "gaussian_splatting/aggregated_count",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_aggregated_count) },
        { "gaussian_splatting/overlap_records_used",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_overlap_records_used) },
        { "gaussian_splatting/overlap_record_budget",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_overlap_record_budget) },

        // Rendering Configuration Monitors
        { "gaussian_splatting/tile_count",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_tile_count) },

        // VRAM Budget Regulation Monitors (Phase 1)
        { "gaussian_splatting/vram_current_usage_mb",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_vram_current_usage_mb) },
        { "gaussian_splatting/vram_budget_mb",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_vram_budget_mb) },
        { "gaussian_splatting/vram_usage_percent",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_vram_usage_percent) },
        { "gaussian_splatting/vram_current_max_chunks",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_vram_current_max_chunks) },
        { "gaussian_splatting/vram_loaded_chunks",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_vram_loaded_chunks) },
        { "gaussian_splatting/vram_evicted_this_frame",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_vram_evicted_this_frame) },
        { "gaussian_splatting/vram_loaded_this_frame",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_vram_loaded_this_frame) },
        { "gaussian_splatting/vram_budget_warning_active",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_vram_budget_warning_active) },
        { "gaussian_splatting/vram_regulation_adjustments",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_vram_regulation_adjustments) },
        { "gaussian_splatting/vram_thrashing_events",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_vram_thrashing_events) },

        // Streaming Core Monitors (Phase 1)
        { "gaussian_splatting/streaming_monitor_ready",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_monitor_ready) },
        { "gaussian_splatting/streaming_runtime_capacity_zero",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_runtime_capacity_zero) },
        { "gaussian_splatting/streaming_runtime_buffer_invalid",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_runtime_buffer_invalid) },
        { "gaussian_splatting/streaming_invalid_camera_inputs",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_invalid_camera_inputs) },
        { "gaussian_splatting/streaming_total_chunks",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_total_chunks) },
        { "gaussian_splatting/streaming_visible_chunks",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_visible_chunks) },
        { "gaussian_splatting/streaming_loaded_chunks",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_loaded_chunks) },
        { "gaussian_splatting/streaming_frustum_culled_chunks",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_frustum_culled_chunks) },
        { "gaussian_splatting/streaming_vram_usage_mb",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_vram_usage_mb) },
        { "gaussian_splatting/streaming_chunks_loaded_this_frame",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_chunks_loaded_this_frame) },
        { "gaussian_splatting/streaming_chunks_evicted_this_frame",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_chunks_evicted_this_frame) },
        { "gaussian_splatting/streaming_visible_count",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_visible_count) },
        { "gaussian_splatting/streaming_buffer_capacity_splats",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_buffer_capacity_splats) },
        { "gaussian_splatting/streaming_effective_splat_count",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_effective_splat_count) },
        { "gaussian_splatting/streaming_visible_change_ratio",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_visible_change_ratio) },
        { "gaussian_splatting/streaming_lod_blend_factor",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_lod_blend_factor) },
        { "gaussian_splatting/streaming_sh_band_level",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_sh_band_level) },
        { "gaussian_splatting/streaming_bytes_uploaded_mb",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_bytes_uploaded_mb) },
        { "gaussian_splatting/streaming_buffer_switches",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_buffer_switches) },
        { "gaussian_splatting/streaming_effective_upload_cap_mb_per_frame",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_effective_upload_cap_mb_per_frame) },
        { "gaussian_splatting/streaming_effective_upload_cap_mb_per_slice",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_effective_upload_cap_mb_per_slice) },
        { "gaussian_splatting/streaming_effective_upload_cap_mb_per_second",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_effective_upload_cap_mb_per_second) },
        { "gaussian_splatting/streaming_effective_vram_budget_mb",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_effective_vram_budget_mb) },
        { "gaussian_splatting/streaming_effective_vram_max_chunks",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_effective_vram_max_chunks) },
        { "gaussian_splatting/streaming_upload_frame_cap_hit",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_upload_frame_cap_hit) },
        { "gaussian_splatting/streaming_upload_bandwidth_cap_hit",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_upload_bandwidth_cap_hit) },
        { "gaussian_splatting/streaming_chunk_load_cap_hit",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_chunk_load_cap_hit) },
        { "gaussian_splatting/streaming_vram_chunk_cap_hit",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_vram_chunk_cap_hit) },
        { "gaussian_splatting/streaming_queue_pressure_active",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_streaming_queue_pressure_active) },

        // LOD System Monitors (Phase 2)
        { "gaussian_splatting/lod_current_level",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_lod_current_level) },
        { "gaussian_splatting/lod_distance_multiplier",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_lod_distance_multiplier) },
        { "gaussian_splatting/lod_target_distance",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_lod_target_distance) },
        { "gaussian_splatting/lod_hysteresis_zone",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_lod_hysteresis_zone) },
        { "gaussian_splatting/lod_blend_distance",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_lod_blend_distance) },
        { "gaussian_splatting/lod_transitions_this_frame",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_lod_transitions_this_frame) },
        { "gaussian_splatting/lod_splat_skip_factor",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_lod_splat_skip_factor) },
        { "gaussian_splatting/lod_opacity_multiplier",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_lod_opacity_multiplier) },
        { "gaussian_splatting/lod_effective_count_after_skip",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_lod_effective_count_after_skip) },
        { "gaussian_splatting/lod_chunk_blend_factors_avg",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_lod_chunk_blend_factors_avg) },
        { "gaussian_splatting/lod_chunks_in_transition",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_lod_chunks_in_transition) },
        { "gaussian_splatting/lod_quality_degradation_active",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_lod_quality_degradation_active) },

        // GPU Memory Stream Monitors (Phase 2)
        { "gaussian_splatting/memory_stream_total_bytes_uploaded_mb",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_memory_stream_total_bytes_uploaded_mb) },
        { "gaussian_splatting/memory_stream_total_bytes_downloaded_mb",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_memory_stream_total_bytes_downloaded_mb) },
        { "gaussian_splatting/memory_stream_buffer_switches",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_memory_stream_buffer_switches) },
        { "gaussian_splatting/memory_stream_stalls",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_memory_stream_stalls) },
        { "gaussian_splatting/memory_stream_stall_percent",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_memory_stream_stall_percent) },
        { "gaussian_splatting/memory_stream_pool_hits",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_memory_stream_pool_hits) },
        { "gaussian_splatting/memory_stream_pool_misses",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_memory_stream_pool_misses) },
        { "gaussian_splatting/memory_stream_pool_hit_rate_pct",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_memory_stream_pool_hit_rate_pct) },
        { "gaussian_splatting/memory_stream_peak_memory_mb",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_memory_stream_peak_memory_mb) },
        { "gaussian_splatting/memory_stream_defrag_count",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_memory_stream_defrag_count) },

        // Chunk Management Monitors (Phase 3)
        { "gaussian_splatting/chunk_prefetch_hits",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_chunk_prefetch_hits) },
        { "gaussian_splatting/chunk_prefetch_misses",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_chunk_prefetch_misses) },
        { "gaussian_splatting/chunk_prefetch_efficiency_pct",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_chunk_prefetch_efficiency_pct) },
        { "gaussian_splatting/chunk_camera_velocity",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_chunk_camera_velocity) },
        { "gaussian_splatting/chunk_average_load_time_ms",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_chunk_average_load_time_ms) },
        { "gaussian_splatting/chunk_upload_queue_depth",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_chunk_upload_queue_depth) },
        { "gaussian_splatting/chunk_pack_jobs_in_flight",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_chunk_pack_jobs_in_flight) },
        { "gaussian_splatting/chunk_total_capacity_mb",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_chunk_total_capacity_mb) },

        // Pack/Upload Timing Monitors (Phase 4.5)
        { "gaussian_splatting/pack_avg_time_ms",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_pack_avg_time_ms) },
        { "gaussian_splatting/pack_max_time_ms",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_pack_max_time_ms) },
        { "gaussian_splatting/pack_jobs_completed",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_pack_jobs_completed) },
        { "gaussian_splatting/upload_mb_this_frame",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_upload_mb_this_frame) },
        { "gaussian_splatting/upload_chunks_this_frame",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_upload_chunks_this_frame) },

        // Advanced LOD Analytics Monitors (Phase 4)
        { "gaussian_splatting/lod_min_chunk_distance",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_lod_min_chunk_distance) },
        { "gaussian_splatting/lod_max_chunk_distance",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_lod_max_chunk_distance) },
        { "gaussian_splatting/lod_avg_chunk_distance",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_lod_avg_chunk_distance) },
        { "gaussian_splatting/lod_reduction_ratio_pct",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_lod_reduction_ratio_pct) },
        { "gaussian_splatting/lod_level_0_chunk_count",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_lod_level_0_chunk_count) },
        { "gaussian_splatting/lod_sh_band_3_chunk_count",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_lod_sh_band_3_chunk_count) },

        // Compression Analytics Monitors (Phase 5)
        { "gaussian_splatting/sh_compression_raw_mb",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_sh_compression_raw_mb) },
        { "gaussian_splatting/sh_compression_compressed_mb",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_sh_compression_compressed_mb) },
        { "gaussian_splatting/sh_compression_ratio_pct",
                callable_mp(this, &GaussianSplattingPerformanceMonitors::_get_sh_compression_ratio_pct) },
    };

    registered_monitor_ids.clear();
    const Vector<Variant> no_args;
    for (const MonitorDefinition &def : monitor_definitions) {
        const StringName monitor_id = StringName(def.name);
        if (p_perf->has_custom_monitor(monitor_id)) {
            p_perf->remove_custom_monitor(monitor_id);
        }
        p_perf->add_custom_monitor(monitor_id, def.callable, no_args);
        registered_monitor_ids.push_back(monitor_id);
    }
}

void GaussianSplattingPerformanceMonitors::initialize_monitors() {
    if (monitors_registered) {
        return;
    }

    Performance *perf = Performance::get_singleton();
    if (!perf) {
        return;
    }
    _register_monitor_definitions(perf);

    monitors_registered = true;
}

void GaussianSplattingPerformanceMonitors::cleanup_monitors() {
    Performance *perf = Performance::get_singleton();
    if (perf) {
        for (const StringName &monitor_id : registered_monitor_ids) {
            if (perf->has_custom_monitor(monitor_id)) {
                perf->remove_custom_monitor(monitor_id);
            }
        }
    }

    registered_monitor_ids.clear();
    monitors_registered = false;
    active_splat_renderer = nullptr;
    registered_splat_renderers.clear();
}

// ============================================================================
// Pipeline Timing Monitor Getters
// ============================================================================

float GaussianSplattingPerformanceMonitors::_get_pipeline_frame_time_ms() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0.0f;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0.0f;
    }
    return _sanitize_ms(snapshot.pipeline_frame_time_ms);
}

float GaussianSplattingPerformanceMonitors::_get_pipeline_cull_time_ms() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0.0f;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0.0f;
    }
    return _sanitize_ms(snapshot.pipeline_cull_time_ms);
}

float GaussianSplattingPerformanceMonitors::_get_pipeline_sort_time_ms() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0.0f;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0.0f;
    }
    return _sanitize_ms(snapshot.pipeline_sort_time_ms);
}

float GaussianSplattingPerformanceMonitors::_get_pipeline_composite_time_ms() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0.0f;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0.0f;
    }
    return _sanitize_ms(snapshot.pipeline_composite_time_ms);
}

float GaussianSplattingPerformanceMonitors::_get_pipeline_binning_time_ms() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0.0f;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0.0f;
    }
    return _sanitize_ms(snapshot.pipeline_binning_time_ms);
}

float GaussianSplattingPerformanceMonitors::_get_pipeline_prefix_time_ms() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0.0f;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0.0f;
    }
    return _sanitize_ms(snapshot.pipeline_prefix_time_ms);
}

float GaussianSplattingPerformanceMonitors::_get_pipeline_raster_time_ms() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0.0f;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0.0f;
    }
    return _sanitize_ms(snapshot.pipeline_raster_time_ms);
}

float GaussianSplattingPerformanceMonitors::_get_pipeline_resolve_time_ms() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0.0f;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0.0f;
    }
    return _sanitize_ms(snapshot.pipeline_resolve_time_ms);
}

// ============================================================================
// CPU Timing Monitor Getters
// ============================================================================

float GaussianSplattingPerformanceMonitors::_get_cpu_setup_time_ms() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0.0f;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0.0f;
    }
    return _sanitize_ms(snapshot.cpu_setup_time_ms);
}

float GaussianSplattingPerformanceMonitors::_get_cpu_sort_submit_ms() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0.0f;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0.0f;
    }
    return _sanitize_ms(snapshot.cpu_sort_submit_ms);
}

float GaussianSplattingPerformanceMonitors::_get_cpu_sort_wait_ms() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0.0f;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0.0f;
    }
    return _sanitize_ms(snapshot.cpu_sort_wait_ms);
}

float GaussianSplattingPerformanceMonitors::_get_cpu_sort_input_build_ms() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0.0f;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0.0f;
    }
    return _sanitize_ms(snapshot.cpu_sort_input_build_ms);
}

int GaussianSplattingPerformanceMonitors::_get_telemetry_active() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0;
    }
    return snapshot.telemetry_active ? 1 : 0;
}

String GaussianSplattingPerformanceMonitors::_get_route_uid() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return String();
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return String();
    }
    return snapshot.route_uid;
}

String GaussianSplattingPerformanceMonitors::_get_sort_route_uid() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return String();
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return String();
    }
    return snapshot.sort_route_uid;
}

// ============================================================================
// Projection Statistics Monitor Getters
// ============================================================================

int GaussianSplattingPerformanceMonitors::_get_visible_splat_count() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0;
    }
    return static_cast<int>(snapshot.visible_splat_count);
}

int GaussianSplattingPerformanceMonitors::_get_total_processed() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0;
    }
    return static_cast<int>(snapshot.total_processed);
}

int GaussianSplattingPerformanceMonitors::_get_projection_success_count() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0;
    }
    return static_cast<int>(snapshot.projection_success_count);
}

float GaussianSplattingPerformanceMonitors::_get_projection_success_rate_pct() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0.0f;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0.0f;
    }
    return snapshot.projection_success_rate_pct;
}

int GaussianSplattingPerformanceMonitors::_get_clip_reject_count() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0;
    }
    return static_cast<int>(snapshot.clip_reject_count);
}

int GaussianSplattingPerformanceMonitors::_get_radius_reject_count() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0;
    }
    return static_cast<int>(snapshot.radius_reject_count);
}

int GaussianSplattingPerformanceMonitors::_get_viewport_reject_count() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0;
    }
    return static_cast<int>(snapshot.viewport_reject_count);
}

// ============================================================================
// Quality Monitor Getters
// ============================================================================

int GaussianSplattingPerformanceMonitors::_get_extreme_aspect_count() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0;
    }
    return static_cast<int>(snapshot.extreme_aspect_count);
}

int GaussianSplattingPerformanceMonitors::_get_index_mismatch_count() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0;
    }
    return static_cast<int>(snapshot.index_mismatch_count);
}

// ============================================================================
// SH Cache Monitor Getters
// ============================================================================

int GaussianSplattingPerformanceMonitors::_get_sh_cache_hits() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0;
    }
    return snapshot.sh_cache_hits;
}

int GaussianSplattingPerformanceMonitors::_get_sh_cache_updates() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0;
    }
    return snapshot.sh_cache_updates;
}

float GaussianSplattingPerformanceMonitors::_get_sh_cache_hit_rate_pct() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0.0f;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0.0f;
    }
    return snapshot.sh_cache_hit_rate_pct;
}

// ============================================================================
// Overflow Monitor Getters
// ============================================================================

int GaussianSplattingPerformanceMonitors::_get_overflow_tile_count() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0;
    }
    return static_cast<int>(snapshot.overflow_tile_count);
}

int GaussianSplattingPerformanceMonitors::_get_clamped_records() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0;
    }
    return static_cast<int>(snapshot.clamped_records);
}

int GaussianSplattingPerformanceMonitors::_get_aggregated_count() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0;
    }
    return static_cast<int>(snapshot.aggregated_count);
}

int GaussianSplattingPerformanceMonitors::_get_overlap_records_used() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0;
    }
    return static_cast<int>(snapshot.overlap_records_used);
}

int GaussianSplattingPerformanceMonitors::_get_overlap_record_budget() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0;
    }
    return static_cast<int>(snapshot.overlap_record_budget);
}

// ============================================================================
// Configuration Monitor Getters
// ============================================================================

int GaussianSplattingPerformanceMonitors::_get_tile_count() const {
    GaussianSplatRenderer *renderer = _get_active_splat_renderer(false);
    if (!renderer) {
        return 0;
    }
    const auto &snapshot = renderer->get_diagnostics_snapshot();
    if (!snapshot.valid) {
        return 0;
    }
    return static_cast<int>(snapshot.tile_count);
}

// ============================================================================
// VRAM Budget Regulation Monitor Getters (Phase 1)
// ============================================================================

float GaussianSplattingPerformanceMonitors::_get_vram_current_usage_mb() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->vram_current_usage_mb : 0.0f;
}

float GaussianSplattingPerformanceMonitors::_get_vram_budget_mb() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->vram_budget_mb : 0.0f;
}

float GaussianSplattingPerformanceMonitors::_get_vram_usage_percent() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->vram_usage_percent : 0.0f;
}

int GaussianSplattingPerformanceMonitors::_get_vram_current_max_chunks() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->vram_current_max_chunks) : 0;
}

int GaussianSplattingPerformanceMonitors::_get_vram_loaded_chunks() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->vram_loaded_chunks) : 0;
}

int GaussianSplattingPerformanceMonitors::_get_vram_evicted_this_frame() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->vram_evicted_this_frame) : 0;
}

int GaussianSplattingPerformanceMonitors::_get_vram_loaded_this_frame() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->vram_loaded_this_frame) : 0;
}

int GaussianSplattingPerformanceMonitors::_get_vram_budget_warning_active() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return (stream && stream->vram_budget_warning_active) ? 1 : 0;
}

int GaussianSplattingPerformanceMonitors::_get_vram_regulation_adjustments() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->vram_regulation_adjustments) : 0;
}

int GaussianSplattingPerformanceMonitors::_get_vram_thrashing_events() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->vram_thrashing_events) : 0;
}

// ============================================================================
// Streaming Core Monitor Getters (Phase 1)
// ============================================================================

int GaussianSplattingPerformanceMonitors::_get_streaming_monitor_ready() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return (stream && stream->streaming_monitor_ready) ? 1 : 0;
}

int GaussianSplattingPerformanceMonitors::_get_streaming_runtime_capacity_zero() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return (stream && stream->streaming_runtime_capacity_zero) ? 1 : 0;
}

int GaussianSplattingPerformanceMonitors::_get_streaming_runtime_buffer_invalid() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return (stream && stream->streaming_runtime_buffer_invalid) ? 1 : 0;
}

int GaussianSplattingPerformanceMonitors::_get_streaming_invalid_camera_inputs() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->streaming_invalid_camera_inputs) : 0;
}

int GaussianSplattingPerformanceMonitors::_get_streaming_total_chunks() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->streaming_total_chunks) : 0;
}

int GaussianSplattingPerformanceMonitors::_get_streaming_visible_chunks() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->streaming_visible_chunks) : 0;
}

int GaussianSplattingPerformanceMonitors::_get_streaming_loaded_chunks() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->streaming_loaded_chunks) : 0;
}

int GaussianSplattingPerformanceMonitors::_get_streaming_frustum_culled_chunks() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->streaming_frustum_culled_chunks) : 0;
}

float GaussianSplattingPerformanceMonitors::_get_streaming_vram_usage_mb() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->streaming_vram_usage_mb : 0.0f;
}

int GaussianSplattingPerformanceMonitors::_get_streaming_chunks_loaded_this_frame() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->streaming_chunks_loaded_this_frame) : 0;
}

int GaussianSplattingPerformanceMonitors::_get_streaming_chunks_evicted_this_frame() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->streaming_chunks_evicted_this_frame) : 0;
}

int GaussianSplattingPerformanceMonitors::_get_streaming_visible_count() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->streaming_visible_count) : 0;
}

int GaussianSplattingPerformanceMonitors::_get_streaming_buffer_capacity_splats() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->streaming_buffer_capacity_splats) : 0;
}

int GaussianSplattingPerformanceMonitors::_get_streaming_effective_splat_count() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->streaming_effective_splat_count) : 0;
}

float GaussianSplattingPerformanceMonitors::_get_streaming_visible_change_ratio() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->streaming_visible_change_ratio : 0.0f;
}

float GaussianSplattingPerformanceMonitors::_get_streaming_lod_blend_factor() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->streaming_lod_blend_factor : 0.0f;
}

int GaussianSplattingPerformanceMonitors::_get_streaming_sh_band_level() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->streaming_sh_band_level) : 0;
}

float GaussianSplattingPerformanceMonitors::_get_streaming_bytes_uploaded_mb() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->streaming_bytes_uploaded_mb : 0.0f;
}

int GaussianSplattingPerformanceMonitors::_get_streaming_buffer_switches() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->streaming_buffer_switches) : 0;
}

float GaussianSplattingPerformanceMonitors::_get_streaming_effective_upload_cap_mb_per_frame() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->streaming_effective_upload_cap_mb_per_frame : 0.0f;
}

float GaussianSplattingPerformanceMonitors::_get_streaming_effective_upload_cap_mb_per_slice() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->streaming_effective_upload_cap_mb_per_slice : 0.0f;
}

float GaussianSplattingPerformanceMonitors::_get_streaming_effective_upload_cap_mb_per_second() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->streaming_effective_upload_cap_mb_per_second : 0.0f;
}

float GaussianSplattingPerformanceMonitors::_get_streaming_effective_vram_budget_mb() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->streaming_effective_vram_budget_mb : 0.0f;
}

int GaussianSplattingPerformanceMonitors::_get_streaming_effective_vram_max_chunks() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->streaming_effective_vram_max_chunks) : 0;
}

int GaussianSplattingPerformanceMonitors::_get_streaming_upload_frame_cap_hit() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return (stream && stream->streaming_upload_frame_cap_hit) ? 1 : 0;
}

int GaussianSplattingPerformanceMonitors::_get_streaming_upload_bandwidth_cap_hit() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return (stream && stream->streaming_upload_bandwidth_cap_hit) ? 1 : 0;
}

int GaussianSplattingPerformanceMonitors::_get_streaming_chunk_load_cap_hit() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return (stream && stream->streaming_chunk_load_cap_hit) ? 1 : 0;
}

int GaussianSplattingPerformanceMonitors::_get_streaming_vram_chunk_cap_hit() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return (stream && stream->streaming_vram_chunk_cap_hit) ? 1 : 0;
}

int GaussianSplattingPerformanceMonitors::_get_streaming_queue_pressure_active() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return (stream && stream->streaming_queue_pressure_active) ? 1 : 0;
}

// ============================================================================
// LOD System Monitor Getters (Phase 2)
// ============================================================================

int GaussianSplattingPerformanceMonitors::_get_lod_current_level() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->lod_current_level : 0;
}

float GaussianSplattingPerformanceMonitors::_get_lod_distance_multiplier() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->lod_distance_multiplier : 0.0f;
}

float GaussianSplattingPerformanceMonitors::_get_lod_target_distance() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->lod_target_distance : 0.0f;
}

float GaussianSplattingPerformanceMonitors::_get_lod_hysteresis_zone() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->lod_hysteresis_zone : 0.0f;
}

float GaussianSplattingPerformanceMonitors::_get_lod_blend_distance() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->lod_blend_distance : 0.0f;
}

int GaussianSplattingPerformanceMonitors::_get_lod_transitions_this_frame() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->lod_transitions_this_frame) : 0;
}

int GaussianSplattingPerformanceMonitors::_get_lod_splat_skip_factor() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->lod_splat_skip_factor) : 1;
}

float GaussianSplattingPerformanceMonitors::_get_lod_opacity_multiplier() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->lod_opacity_multiplier : 1.0f;
}

int GaussianSplattingPerformanceMonitors::_get_lod_effective_count_after_skip() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->lod_effective_count_after_skip) : 0;
}

float GaussianSplattingPerformanceMonitors::_get_lod_chunk_blend_factors_avg() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->lod_chunk_blend_factors_avg : 0.0f;
}

int GaussianSplattingPerformanceMonitors::_get_lod_chunks_in_transition() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->lod_chunks_in_transition) : 0;
}

int GaussianSplattingPerformanceMonitors::_get_lod_quality_degradation_active() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return (stream && stream->lod_quality_degradation_active) ? 1 : 0;
}

// ============================================================================
// GPU Memory Stream Monitor Getters (Phase 2)
// ============================================================================

float GaussianSplattingPerformanceMonitors::_get_memory_stream_total_bytes_uploaded_mb() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->memory_stream_total_bytes_uploaded_mb : 0.0f;
}

float GaussianSplattingPerformanceMonitors::_get_memory_stream_total_bytes_downloaded_mb() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->memory_stream_total_bytes_downloaded_mb : 0.0f;
}

int GaussianSplattingPerformanceMonitors::_get_memory_stream_buffer_switches() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->memory_stream_buffer_switches) : 0;
}

int GaussianSplattingPerformanceMonitors::_get_memory_stream_stalls() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->memory_stream_stalls) : 0;
}

float GaussianSplattingPerformanceMonitors::_get_memory_stream_stall_percent() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->memory_stream_stall_percent : 0.0f;
}

int GaussianSplattingPerformanceMonitors::_get_memory_stream_pool_hits() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->memory_stream_pool_hits) : 0;
}

int GaussianSplattingPerformanceMonitors::_get_memory_stream_pool_misses() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->memory_stream_pool_misses) : 0;
}

float GaussianSplattingPerformanceMonitors::_get_memory_stream_pool_hit_rate_pct() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->memory_stream_pool_hit_rate_pct : 0.0f;
}

float GaussianSplattingPerformanceMonitors::_get_memory_stream_peak_memory_mb() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->memory_stream_peak_memory_mb : 0.0f;
}

int GaussianSplattingPerformanceMonitors::_get_memory_stream_defrag_count() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->memory_stream_defrag_count) : 0;
}

// ============================================================================
// Chunk Management Monitor Getters (Phase 3)
// ============================================================================

int GaussianSplattingPerformanceMonitors::_get_chunk_prefetch_hits() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->chunk_prefetch_hits) : 0;
}

int GaussianSplattingPerformanceMonitors::_get_chunk_prefetch_misses() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->chunk_prefetch_misses) : 0;
}

float GaussianSplattingPerformanceMonitors::_get_chunk_prefetch_efficiency_pct() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->chunk_prefetch_efficiency_pct : 0.0f;
}

float GaussianSplattingPerformanceMonitors::_get_chunk_camera_velocity() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->chunk_camera_velocity : 0.0f;
}

float GaussianSplattingPerformanceMonitors::_get_chunk_average_load_time_ms() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->chunk_average_load_time_ms : 0.0f;
}

int GaussianSplattingPerformanceMonitors::_get_chunk_upload_queue_depth() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->chunk_upload_queue_depth) : 0;
}

int GaussianSplattingPerformanceMonitors::_get_chunk_pack_jobs_in_flight() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->chunk_pack_jobs_in_flight) : 0;
}

float GaussianSplattingPerformanceMonitors::_get_chunk_total_capacity_mb() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->chunk_total_capacity_mb : 0.0f;
}

// Pack/Upload Timing Monitor Getters (Phase 4.5)
float GaussianSplattingPerformanceMonitors::_get_pack_avg_time_ms() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->pack_avg_time_ms : 0.0f;
}

float GaussianSplattingPerformanceMonitors::_get_pack_max_time_ms() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->pack_max_time_ms : 0.0f;
}

int GaussianSplattingPerformanceMonitors::_get_pack_jobs_completed() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->pack_jobs_completed) : 0;
}

float GaussianSplattingPerformanceMonitors::_get_upload_mb_this_frame() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->upload_mb_this_frame : 0.0f;
}

int GaussianSplattingPerformanceMonitors::_get_upload_chunks_this_frame() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->upload_chunks_this_frame) : 0;
}

// Advanced LOD Analytics Monitor Getters (Phase 4)
float GaussianSplattingPerformanceMonitors::_get_lod_min_chunk_distance() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->lod_min_chunk_distance : 0.0f;
}

float GaussianSplattingPerformanceMonitors::_get_lod_max_chunk_distance() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->lod_max_chunk_distance : 0.0f;
}

float GaussianSplattingPerformanceMonitors::_get_lod_avg_chunk_distance() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->lod_avg_chunk_distance : 0.0f;
}

float GaussianSplattingPerformanceMonitors::_get_lod_reduction_ratio_pct() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->lod_reduction_ratio_pct : 0.0f;
}

int GaussianSplattingPerformanceMonitors::_get_lod_level_0_chunk_count() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->lod_level_0_chunk_count) : 0;
}

int GaussianSplattingPerformanceMonitors::_get_lod_sh_band_3_chunk_count() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? static_cast<int>(stream->lod_sh_band_3_chunk_count) : 0;
}

// Compression Analytics Monitor Getters (Phase 5)
float GaussianSplattingPerformanceMonitors::_get_sh_compression_raw_mb() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->sh_compression_raw_mb : 0.0f;
}

float GaussianSplattingPerformanceMonitors::_get_sh_compression_compressed_mb() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->sh_compression_compressed_mb : 0.0f;
}

float GaussianSplattingPerformanceMonitors::_get_sh_compression_ratio_pct() const {
	const GaussianSplatDiagnosticsSnapshot::StreamingSnapshot *stream = _get_valid_streaming_snapshot(_get_active_splat_renderer(false));
	return stream ? stream->sh_compression_ratio_pct : 0.0f;
}

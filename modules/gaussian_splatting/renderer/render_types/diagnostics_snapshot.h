/**
 * @file diagnostics_snapshot.h
 * @brief Canonical per-frame diagnostics snapshot for the Gaussian Splatting renderer.
 *
 * This struct is the single source of truth for all per-frame timing, stats,
 * and metadata produced by the rendering pipeline. Every consumer -- performance
 * monitors, HUD overlays, and the get_render_stats() dictionary API -- reads
 * from this snapshot instead of maintaining its own timing mirrors.
 *
 * Populated once per frame by the renderer pipeline, then treated as read-only
 * by all downstream consumers until the next frame's clear()/populate cycle.
 */

#ifndef GAUSSIAN_DIAGNOSTICS_SNAPSHOT_H
#define GAUSSIAN_DIAGNOSTICS_SNAPSHOT_H

#include "core/string/string_name.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "core/variant/dictionary.h"
#include <cstdint>

/**
 * @struct GaussianSplatDiagnosticsSnapshot
 * @brief One-stop per-frame snapshot of every diagnostic metric.
 *
 * Fields are grouped into logical sections that map 1-to-1 onto the
 * categories previously scattered across PerformanceMetrics, DebugState,
 * StageMetrics, FrameState, and the performance-monitor getters.
 *
 * All timing values are in milliseconds. Pipeline timings are 0.0 when
 * telemetry is inactive or the measurement is unavailable.
 */
struct GaussianSplatDiagnosticsSnapshot {
	struct StreamingSnapshot {
		// VRAM budget regulation
		float vram_current_usage_mb = 0.0f;
		float vram_budget_mb = 0.0f;
		float vram_usage_percent = 0.0f;
		uint32_t vram_current_max_chunks = 0;
		uint32_t vram_loaded_chunks = 0;
		uint32_t vram_evicted_this_frame = 0;
		uint32_t vram_loaded_this_frame = 0;
		bool vram_budget_warning_active = false;
		uint32_t vram_regulation_adjustments = 0;
		uint32_t vram_thrashing_events = 0;

		// Streaming core
		bool streaming_monitor_ready = false;
		bool streaming_runtime_capacity_zero = false;
		bool streaming_runtime_buffer_invalid = false;
		uint32_t streaming_invalid_camera_inputs = 0;
		uint32_t streaming_total_chunks = 0;
		uint32_t streaming_visible_chunks = 0;
		uint32_t streaming_loaded_chunks = 0;
		uint32_t streaming_frustum_culled_chunks = 0;
		float streaming_vram_usage_mb = 0.0f;
		uint32_t streaming_chunks_loaded_this_frame = 0;
		uint32_t streaming_chunks_evicted_this_frame = 0;
		uint32_t streaming_visible_count = 0;
		uint32_t streaming_buffer_capacity_splats = 0;
		uint32_t streaming_effective_splat_count = 0;
		float streaming_visible_change_ratio = 0.0f;
		float streaming_lod_blend_factor = 0.0f;
		uint32_t streaming_sh_band_level = 0;
		float streaming_bytes_uploaded_mb = 0.0f;
		uint32_t streaming_buffer_switches = 0;
		float streaming_effective_upload_cap_mb_per_frame = 0.0f;
		float streaming_effective_upload_cap_mb_per_slice = 0.0f;
		float streaming_effective_upload_cap_mb_per_second = 0.0f;
		float streaming_effective_vram_budget_mb = 0.0f;
		uint32_t streaming_effective_vram_max_chunks = 0;
		bool streaming_upload_frame_cap_hit = false;
		bool streaming_upload_bandwidth_cap_hit = false;
		bool streaming_chunk_load_cap_hit = false;
		bool streaming_vram_chunk_cap_hit = false;
		bool streaming_queue_pressure_active = false;

		// LOD system
		int32_t lod_current_level = 0;
		float lod_distance_multiplier = 1.0f;
		float lod_target_distance = 0.0f;
		float lod_hysteresis_zone = 0.0f;
		float lod_blend_distance = 0.0f;
		uint32_t lod_transitions_this_frame = 0;
		uint32_t lod_splat_skip_factor = 1;
		float lod_opacity_multiplier = 1.0f;
		uint32_t lod_effective_count_after_skip = 0;
		float lod_chunk_blend_factors_avg = 0.0f;
		uint32_t lod_chunks_in_transition = 0;
		bool lod_quality_degradation_active = false;

		// Memory stream
		float memory_stream_total_bytes_uploaded_mb = 0.0f;
		float memory_stream_total_bytes_downloaded_mb = 0.0f;
		uint32_t memory_stream_buffer_switches = 0;
		uint32_t memory_stream_stalls = 0;
		float memory_stream_stall_percent = 0.0f;
		uint32_t memory_stream_pool_hits = 0;
		uint32_t memory_stream_pool_misses = 0;
		float memory_stream_pool_hit_rate_pct = 0.0f;
		float memory_stream_peak_memory_mb = 0.0f;
		uint32_t memory_stream_defrag_count = 0;

		// Chunk management
		uint32_t chunk_prefetch_hits = 0;
		uint32_t chunk_prefetch_misses = 0;
		float chunk_prefetch_efficiency_pct = 0.0f;
		float chunk_camera_velocity = 0.0f;
		float chunk_average_load_time_ms = 0.0f;
		uint32_t chunk_upload_queue_depth = 0;
		uint32_t chunk_pack_jobs_in_flight = 0;
		float chunk_total_capacity_mb = 0.0f;

		// Pack/upload timing
		float pack_avg_time_ms = 0.0f;
		float pack_max_time_ms = 0.0f;
		uint32_t pack_jobs_completed = 0;
		float upload_mb_this_frame = 0.0f;
		uint32_t upload_chunks_this_frame = 0;

		// Advanced LOD analytics
		float lod_min_chunk_distance = 0.0f;
		float lod_max_chunk_distance = 0.0f;
		float lod_avg_chunk_distance = 0.0f;
		float lod_reduction_ratio_pct = 0.0f;
		uint32_t lod_level_0_chunk_count = 0;
		uint32_t lod_sh_band_3_chunk_count = 0;

		// SH compression analytics
		float sh_compression_raw_mb = 0.0f;
		float sh_compression_compressed_mb = 0.0f;
		float sh_compression_ratio_pct = 0.0f;

		void clear() {
			*this = StreamingSnapshot();
		}

		void to_dictionary(Dictionary &d) const {
			d["vram_current_usage_mb"] = vram_current_usage_mb;
			d["vram_budget_mb"] = vram_budget_mb;
			d["vram_usage_percent"] = vram_usage_percent;
			d["vram_current_max_chunks"] = static_cast<int64_t>(vram_current_max_chunks);
			d["vram_loaded_chunks"] = static_cast<int64_t>(vram_loaded_chunks);
			d["vram_evicted_this_frame"] = static_cast<int64_t>(vram_evicted_this_frame);
			d["vram_loaded_this_frame"] = static_cast<int64_t>(vram_loaded_this_frame);
			d["vram_budget_warning_active"] = vram_budget_warning_active;
			d["vram_regulation_adjustments"] = static_cast<int64_t>(vram_regulation_adjustments);
			d["vram_thrashing_events"] = static_cast<int64_t>(vram_thrashing_events);

			d["streaming_monitor_ready"] = streaming_monitor_ready;
			d["streaming_runtime_capacity_zero"] = streaming_runtime_capacity_zero;
			d["streaming_runtime_buffer_invalid"] = streaming_runtime_buffer_invalid;
			d["streaming_invalid_camera_inputs"] = static_cast<int64_t>(streaming_invalid_camera_inputs);
			d["streaming_total_chunks"] = static_cast<int64_t>(streaming_total_chunks);
			d["streaming_visible_chunks"] = static_cast<int64_t>(streaming_visible_chunks);
			d["streaming_loaded_chunks"] = static_cast<int64_t>(streaming_loaded_chunks);
			d["streaming_frustum_culled_chunks"] = static_cast<int64_t>(streaming_frustum_culled_chunks);
			d["streaming_vram_usage_mb"] = streaming_vram_usage_mb;
			d["streaming_chunks_loaded_this_frame"] = static_cast<int64_t>(streaming_chunks_loaded_this_frame);
			d["streaming_chunks_evicted_this_frame"] = static_cast<int64_t>(streaming_chunks_evicted_this_frame);
			d["streaming_visible_count"] = static_cast<int64_t>(streaming_visible_count);
			d["streaming_buffer_capacity_splats"] = static_cast<int64_t>(streaming_buffer_capacity_splats);
			d["streaming_effective_splat_count"] = static_cast<int64_t>(streaming_effective_splat_count);
			d["streaming_visible_change_ratio"] = streaming_visible_change_ratio;
			d["streaming_lod_blend_factor"] = streaming_lod_blend_factor;
			d["streaming_sh_band_level"] = static_cast<int64_t>(streaming_sh_band_level);
			d["streaming_bytes_uploaded_mb"] = streaming_bytes_uploaded_mb;
			d["streaming_buffer_switches"] = static_cast<int64_t>(streaming_buffer_switches);
			d["streaming_effective_upload_cap_mb_per_frame"] = streaming_effective_upload_cap_mb_per_frame;
			d["streaming_effective_upload_cap_mb_per_slice"] = streaming_effective_upload_cap_mb_per_slice;
			d["streaming_effective_upload_cap_mb_per_second"] = streaming_effective_upload_cap_mb_per_second;
			d["streaming_effective_vram_budget_mb"] = streaming_effective_vram_budget_mb;
			d["streaming_effective_vram_max_chunks"] = static_cast<int64_t>(streaming_effective_vram_max_chunks);
			d["streaming_upload_frame_cap_hit"] = streaming_upload_frame_cap_hit;
			d["streaming_upload_bandwidth_cap_hit"] = streaming_upload_bandwidth_cap_hit;
			d["streaming_chunk_load_cap_hit"] = streaming_chunk_load_cap_hit;
			d["streaming_vram_chunk_cap_hit"] = streaming_vram_chunk_cap_hit;
			d["streaming_queue_pressure_active"] = streaming_queue_pressure_active;

			d["lod_current_level"] = static_cast<int64_t>(lod_current_level);
			d["lod_distance_multiplier"] = lod_distance_multiplier;
			d["lod_target_distance"] = lod_target_distance;
			d["lod_hysteresis_zone"] = lod_hysteresis_zone;
			d["lod_blend_distance"] = lod_blend_distance;
			d["lod_transitions_this_frame"] = static_cast<int64_t>(lod_transitions_this_frame);
			d["lod_splat_skip_factor"] = static_cast<int64_t>(lod_splat_skip_factor);
			d["lod_opacity_multiplier"] = lod_opacity_multiplier;
			d["lod_effective_count_after_skip"] = static_cast<int64_t>(lod_effective_count_after_skip);
			d["lod_chunk_blend_factors_avg"] = lod_chunk_blend_factors_avg;
			d["lod_chunks_in_transition"] = static_cast<int64_t>(lod_chunks_in_transition);
			d["lod_quality_degradation_active"] = lod_quality_degradation_active;

			d["memory_stream_total_bytes_uploaded_mb"] = memory_stream_total_bytes_uploaded_mb;
			d["memory_stream_total_bytes_downloaded_mb"] = memory_stream_total_bytes_downloaded_mb;
			d["memory_stream_buffer_switches"] = static_cast<int64_t>(memory_stream_buffer_switches);
			d["memory_stream_stalls"] = static_cast<int64_t>(memory_stream_stalls);
			d["memory_stream_stall_percent"] = memory_stream_stall_percent;
			d["memory_stream_pool_hits"] = static_cast<int64_t>(memory_stream_pool_hits);
			d["memory_stream_pool_misses"] = static_cast<int64_t>(memory_stream_pool_misses);
			d["memory_stream_pool_hit_rate_pct"] = memory_stream_pool_hit_rate_pct;
			d["memory_stream_peak_memory_mb"] = memory_stream_peak_memory_mb;
			d["memory_stream_defrag_count"] = static_cast<int64_t>(memory_stream_defrag_count);

			d["chunk_prefetch_hits"] = static_cast<int64_t>(chunk_prefetch_hits);
			d["chunk_prefetch_misses"] = static_cast<int64_t>(chunk_prefetch_misses);
			d["chunk_prefetch_efficiency_pct"] = chunk_prefetch_efficiency_pct;
			d["chunk_camera_velocity"] = chunk_camera_velocity;
			d["chunk_average_load_time_ms"] = chunk_average_load_time_ms;
			d["chunk_upload_queue_depth"] = static_cast<int64_t>(chunk_upload_queue_depth);
			d["chunk_pack_jobs_in_flight"] = static_cast<int64_t>(chunk_pack_jobs_in_flight);
			d["chunk_total_capacity_mb"] = chunk_total_capacity_mb;

			d["pack_avg_time_ms"] = pack_avg_time_ms;
			d["pack_max_time_ms"] = pack_max_time_ms;
			d["pack_jobs_completed"] = static_cast<int64_t>(pack_jobs_completed);
			d["upload_mb_this_frame"] = upload_mb_this_frame;
			d["upload_chunks_this_frame"] = static_cast<int64_t>(upload_chunks_this_frame);

			d["lod_min_chunk_distance"] = lod_min_chunk_distance;
			d["lod_max_chunk_distance"] = lod_max_chunk_distance;
			d["lod_avg_chunk_distance"] = lod_avg_chunk_distance;
			d["lod_reduction_ratio_pct"] = lod_reduction_ratio_pct;
			d["lod_level_0_chunk_count"] = static_cast<int64_t>(lod_level_0_chunk_count);
			d["lod_sh_band_3_chunk_count"] = static_cast<int64_t>(lod_sh_band_3_chunk_count);

			d["sh_compression_raw_mb"] = sh_compression_raw_mb;
			d["sh_compression_compressed_mb"] = sh_compression_compressed_mb;
			d["sh_compression_ratio_pct"] = sh_compression_ratio_pct;
		}
	};

	// ---------------------------------------------------------------
	// Pipeline Stage Timing (CPU-observed, not GPU timestamps)
	// ---------------------------------------------------------------
	float pipeline_frame_time_ms = 0.0f;
	float pipeline_cull_time_ms = 0.0f;
	float pipeline_sort_time_ms = 0.0f;      ///< CPU-observed; 0.0 when CPU fallback is active.
	float pipeline_binning_time_ms = 0.0f;
	float pipeline_prefix_time_ms = 0.0f;
	float pipeline_raster_time_ms = 0.0f;
	float pipeline_resolve_time_ms = 0.0f;
	float pipeline_composite_time_ms = 0.0f;

	// ---------------------------------------------------------------
	// CPU Timing
	// ---------------------------------------------------------------
	float cpu_setup_time_ms = 0.0f;
	float cpu_sort_submit_ms = 0.0f;
	float cpu_sort_wait_ms = 0.0f;
	float cpu_sort_input_build_ms = 0.0f;

	// ---------------------------------------------------------------
	// Sort Metadata
	// ---------------------------------------------------------------
	bool sort_used_gpu = false;
	bool sort_used_cpu_fallback = false;
	StringName sort_algorithm;
	uint32_t sort_element_count = 0;

	// ---------------------------------------------------------------
	// Visibility / Projection Stats
	// ---------------------------------------------------------------
	uint32_t visible_splat_count = 0;
	uint32_t total_processed = 0;
	uint32_t projection_success_count = 0;
	float projection_success_rate_pct = 0.0f;
	uint32_t clip_reject_count = 0;
	uint32_t radius_reject_count = 0;
	uint32_t viewport_reject_count = 0;
	uint32_t extreme_aspect_count = 0;
	uint32_t index_mismatch_count = 0;

	// ---------------------------------------------------------------
	// Tile Stats
	// ---------------------------------------------------------------
	uint32_t tile_count = 0;
	uint32_t overflow_tile_count = 0;
	uint32_t clamped_records = 0;
	uint32_t aggregated_count = 0;
	uint32_t overlap_records_used = 0;
	uint32_t overlap_record_budget = 0;

	// ---------------------------------------------------------------
	// SH Cache
	// ---------------------------------------------------------------
	uint32_t sh_cache_hits = 0;
	uint32_t sh_cache_updates = 0;
	float sh_cache_hit_rate_pct = 0.0f;

	// ---------------------------------------------------------------
	// Stage Metrics (from pipeline stage outputs)
	// ---------------------------------------------------------------

	// Cull stage
	uint32_t stage_cull_candidate_count = 0;
	uint32_t stage_cull_visible_count = 0;

	// Sort stage
	bool stage_sort_did_sort = false;
	uint32_t stage_sort_input_count = 0;
	uint32_t stage_sort_sorted_count = 0;

	// Raster stage
	bool stage_raster_reused_cached = false;
	bool stage_raster_painterly_active = false;

	// Composite stage
	bool stage_composite_executed = false;

	// ---------------------------------------------------------------
	// Frame Metadata
	// ---------------------------------------------------------------
	uint64_t frame_index = 0;
	float frame_time_ms = 0.0f;         ///< Wall-clock frame-to-frame time.
	bool telemetry_active = false;
	String route_uid;
	String sort_route_uid;
	String data_source;

	// ---------------------------------------------------------------
	// Validity
	// ---------------------------------------------------------------
	bool valid = false;                  ///< True once the snapshot has been populated this frame.
	bool stage_metrics_valid = false;    ///< True when the stage metrics section is populated.
	bool streaming_valid = false;        ///< True when streaming/VRAM/LOD sections are populated for this frame.
	StreamingSnapshot streaming;

	// -------------------------------------------------------------------
	// Lifecycle helpers
	// -------------------------------------------------------------------

	/**
	 * Reset every field to its default value, marking the snapshot invalid.
	 * Call at the start of a new frame before populating fresh data.
	 */
	void clear() {
		// Pipeline stage timing
		pipeline_frame_time_ms = 0.0f;
		pipeline_cull_time_ms = 0.0f;
		pipeline_sort_time_ms = 0.0f;
		pipeline_binning_time_ms = 0.0f;
		pipeline_prefix_time_ms = 0.0f;
		pipeline_raster_time_ms = 0.0f;
		pipeline_resolve_time_ms = 0.0f;
		pipeline_composite_time_ms = 0.0f;

		// CPU timing
		cpu_setup_time_ms = 0.0f;
		cpu_sort_submit_ms = 0.0f;
		cpu_sort_wait_ms = 0.0f;
		cpu_sort_input_build_ms = 0.0f;

		// Sort metadata
		sort_used_gpu = false;
		sort_used_cpu_fallback = false;
		sort_algorithm = StringName();
		sort_element_count = 0;

		// Visibility / Projection
		visible_splat_count = 0;
		total_processed = 0;
		projection_success_count = 0;
		projection_success_rate_pct = 0.0f;
		clip_reject_count = 0;
		radius_reject_count = 0;
		viewport_reject_count = 0;
		extreme_aspect_count = 0;
		index_mismatch_count = 0;

		// Tile stats
		tile_count = 0;
		overflow_tile_count = 0;
		clamped_records = 0;
		aggregated_count = 0;
		overlap_records_used = 0;
		overlap_record_budget = 0;

		// SH Cache
		sh_cache_hits = 0;
		sh_cache_updates = 0;
		sh_cache_hit_rate_pct = 0.0f;

		// Stage metrics
		stage_cull_candidate_count = 0;
		stage_cull_visible_count = 0;

		stage_sort_did_sort = false;
		stage_sort_input_count = 0;
		stage_sort_sorted_count = 0;

		stage_raster_reused_cached = false;
		stage_raster_painterly_active = false;

		stage_composite_executed = false;

		// Frame metadata
		frame_index = 0;
		frame_time_ms = 0.0f;
		telemetry_active = false;
		route_uid = String();
		sort_route_uid = String();
		data_source = String();

		// Validity
		valid = false;
		stage_metrics_valid = false;
		streaming_valid = false;
		streaming.clear();
	}

	// -------------------------------------------------------------------
	// Dictionary export
	// -------------------------------------------------------------------

	/**
	 * Export every field to a Godot Dictionary.
	 *
	 * Intended to replace get_render_stats() so that scripts and tooling
	 * receive a single authoritative dictionary per frame.
	 */
	Dictionary to_dictionary() const {
		Dictionary d;

		// Pipeline stage timing
		d["pipeline_frame_time_ms"] = pipeline_frame_time_ms;
		d["pipeline_cull_time_ms"] = pipeline_cull_time_ms;
		d["pipeline_sort_time_ms"] = pipeline_sort_time_ms;
		d["pipeline_binning_time_ms"] = pipeline_binning_time_ms;
		d["pipeline_prefix_time_ms"] = pipeline_prefix_time_ms;
		d["pipeline_raster_time_ms"] = pipeline_raster_time_ms;
		d["pipeline_resolve_time_ms"] = pipeline_resolve_time_ms;
		d["pipeline_composite_time_ms"] = pipeline_composite_time_ms;

		// CPU timing
		d["cpu_setup_time_ms"] = cpu_setup_time_ms;
		d["cpu_sort_submit_ms"] = cpu_sort_submit_ms;
		d["cpu_sort_wait_ms"] = cpu_sort_wait_ms;
		d["cpu_sort_input_build_ms"] = cpu_sort_input_build_ms;

		// Sort metadata
		d["sort_used_gpu"] = sort_used_gpu;
		d["sort_used_cpu_fallback"] = sort_used_cpu_fallback;
		d["sort_algorithm"] = sort_algorithm;
		d["sort_element_count"] = sort_element_count;

		// Visibility / Projection
		d["visible_splat_count"] = visible_splat_count;
		d["total_processed"] = total_processed;
		d["projection_success_count"] = projection_success_count;
		d["projection_success_rate_pct"] = projection_success_rate_pct;
		d["clip_reject_count"] = clip_reject_count;
		d["radius_reject_count"] = radius_reject_count;
		d["viewport_reject_count"] = viewport_reject_count;
		d["extreme_aspect_count"] = extreme_aspect_count;
		d["index_mismatch_count"] = index_mismatch_count;

		// Tile stats
		d["tile_count"] = tile_count;
		d["overflow_tile_count"] = overflow_tile_count;
		d["clamped_records"] = clamped_records;
		d["aggregated_count"] = aggregated_count;
		d["overlap_records_used"] = overlap_records_used;
		d["overlap_record_budget"] = overlap_record_budget;

		// SH Cache
		d["sh_cache_hits"] = sh_cache_hits;
		d["sh_cache_updates"] = sh_cache_updates;
		d["sh_cache_hit_rate_pct"] = sh_cache_hit_rate_pct;

		// Stage metrics
		d["stage_cull_candidate_count"] = stage_cull_candidate_count;
		d["stage_cull_visible_count"] = stage_cull_visible_count;

		d["stage_sort_did_sort"] = stage_sort_did_sort;
		d["stage_sort_input_count"] = stage_sort_input_count;
		d["stage_sort_sorted_count"] = stage_sort_sorted_count;

		d["stage_raster_reused_cached"] = stage_raster_reused_cached;
		d["stage_raster_painterly_active"] = stage_raster_painterly_active;

		d["stage_composite_executed"] = stage_composite_executed;

		// Frame metadata
		d["frame_index"] = frame_index;
		d["frame_time_ms"] = frame_time_ms;
		d["telemetry_active"] = telemetry_active;
		d["route_uid"] = route_uid;
		d["sort_route_uid"] = sort_route_uid;
		d["data_source"] = data_source;

			// Validity
			d["valid"] = valid;
			d["stage_metrics_valid"] = stage_metrics_valid;
			d["streaming_valid"] = streaming_valid;
			streaming.to_dictionary(d);

			return d;
		}
	};

#endif // GAUSSIAN_DIAGNOSTICS_SNAPSHOT_H

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
	String data_source;

	// ---------------------------------------------------------------
	// Validity
	// ---------------------------------------------------------------
	bool valid = false;                  ///< True once the snapshot has been populated this frame.
	bool stage_metrics_valid = false;    ///< True when the stage metrics section is populated.

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
		data_source = String();

		// Validity
		valid = false;
		stage_metrics_valid = false;
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
		d["data_source"] = data_source;

		// Validity
		d["valid"] = valid;
		d["stage_metrics_valid"] = stage_metrics_valid;

		return d;
	}
};

#endif // GAUSSIAN_DIAGNOSTICS_SNAPSHOT_H

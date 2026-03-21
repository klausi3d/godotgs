/**
 * @file render_performance_types.h
 * @brief Performance metrics and settings type definitions.
 *
 * Standalone types for performance monitoring and quality configuration.
 * Extracted from GaussianSplatRenderer so orchestrators and diagnostic
 * systems can depend on narrow contracts.
 */

#ifndef GAUSSIAN_RENDER_PERFORMANCE_TYPES_H
#define GAUSSIAN_RENDER_PERFORMANCE_TYPES_H

#include "core/string/string_name.h"
#include "core/string/ustring.h"
#include "render_pipeline_io_types.h"
#include <cstdint>

namespace GaussianRenderPerformance {

struct PerformanceSettings {
	int max_splats = 5000000;
};

/**
 * @struct SortFrameMetrics
 * @brief Per-frame sorting performance metrics.
 *
 * Captures timing and algorithm selection data for a single frame's
 * depth sorting pass. Used for performance monitoring and debugging.
 */
struct SortFrameMetrics {
	uint32_t frame_index = 0;       ///< Frame number for correlation.
	uint32_t element_count = 0;     ///< Number of splats sorted.
	float total_ms = 0.0f;          ///< Total sorting time in milliseconds.
	float gpu_ms = 0.0f;            ///< GPU-side sorting time.
	float cpu_ms = 0.0f;            ///< CPU-side sorting time (if fallback).
	float cpu_selection_ms = 0.0f;  ///< Time spent preparing sort input buffers (ms).
	StringName algorithm;           ///< Name of the sorting algorithm used.
	bool used_gpu = false;          ///< True if GPU sorting was used.
	bool used_cpu_fallback = false; ///< True if CPU fallback was triggered.
	bool used_hybrid = false;       ///< Reserved for future hybrid GPU/CPU sorting.
};

/**
 * @struct DataSourceInfo
 * @brief Tracks which data source is feeding the renderer.
 *
 * The three fields previously lived in the monolithic PerformanceMetrics
 * struct.  They are actively written by apply_data_source_plan() and
 * read by the diagnostics snapshot and telemetry dictionaries.
 */
struct DataSourceInfo {
	String data_source = GaussianRenderPipeline::SplatDataSource::kSourceNone;
	bool using_real_data = false;
	String data_source_error;
};

struct PerformanceState {
	DataSourceInfo data_source_info;
	SortFrameMetrics last_sort_metrics;
	bool last_sort_metrics_valid = false;
};

} // namespace GaussianRenderPerformance

#endif // GAUSSIAN_RENDER_PERFORMANCE_TYPES_H

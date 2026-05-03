/**************************************************************************/
/*  test_sorter_metrics_metadata.h                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "../renderer/gpu_sorter.h"

#include "tests/test_macros.h"

namespace TestGaussianSplatting {

TEST_CASE("[GaussianSplatting][Sorter] Metrics preserve direct sort metadata") {
	SortingMetricsCollector collector;

	collector.record_sort(123u, 0.5f, true, 64u, 4u, 16u, false, false, true);
	const SortingMetrics metrics = collector.get_metrics();

	CHECK_EQ(metrics.last_element_count, 123u);
	CHECK(metrics.last_element_count_known);
	CHECK_EQ(metrics.last_key_bits, 64u);
	CHECK_EQ(metrics.last_radix_bits, 4u);
	CHECK_EQ(metrics.last_pass_count, 16u);
	CHECK_FALSE(metrics.last_sort_indirect);
	CHECK_FALSE(metrics.last_sort_async);
	CHECK_EQ(metrics.total_sorts, 1u);
	CHECK_EQ(metrics.async_sorts, 0u);
}

TEST_CASE("[GaussianSplatting][Sorter] Metrics keep indirect element count explicit") {
	SortingMetricsCollector collector;

	collector.record_async_sort(0u, 0.25f, 64u, 4u, 16u, true, false);
	const SortingMetrics metrics = collector.get_metrics();

	CHECK_EQ(metrics.last_element_count, 0u);
	CHECK_FALSE(metrics.last_element_count_known);
	CHECK_EQ(metrics.last_key_bits, 64u);
	CHECK_EQ(metrics.last_radix_bits, 4u);
	CHECK_EQ(metrics.last_pass_count, 16u);
	CHECK(metrics.last_sort_indirect);
	CHECK(metrics.last_sort_async);
	CHECK_EQ(metrics.total_sorts, 1u);
	CHECK_EQ(metrics.async_sorts, 1u);
}

} // namespace TestGaussianSplatting

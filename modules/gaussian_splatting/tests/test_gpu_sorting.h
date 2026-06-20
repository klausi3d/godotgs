/**************************************************************************/
/*  test_gpu_sorting.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "../renderer/gpu_sorter.h"
#include "../renderer/gpu_sorting_config.h"
#include "../core/gaussian_data.h"
#include "core/math/math_funcs.h"
#include "core/math/random_number_generator.h"
#include "core/os/os.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering_server.h"
#include "tests/test_macros.h"
#include <algorithm>

namespace TestGaussianSplatting {

// Helper to create storage buffer from typed data
template<typename T>
static RID create_storage_buffer(RenderingDevice *rd, const LocalVector<T> &data) {
	Vector<uint8_t> bytes;
	bytes.resize(data.size() * sizeof(T));
	memcpy(bytes.ptrw(), data.ptr(), bytes.size());
	return rd->storage_buffer_create(bytes.size(), bytes);
}

TEST_CASE("[GaussianSplatting][RequiresGPU] GPU Bitonic Sorting") {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!rd) {
		RenderingServer *rs = RenderingServer::get_singleton();
		if (rs) {
			rd = rs->create_local_rendering_device();
		}
	}

	if (!rd) {
		MESSAGE("Skipping GPU sorting tests - no RenderingDevice available");
		return;
	}

	SUBCASE("Initialize GPU sorter") {
		Ref<BitonicSort> sorter;
		sorter.instantiate();

		Error err = sorter->initialize(rd, 10000);
		CHECK(err == OK);
		CHECK(sorter->get_max_elements() == 10000);
	}

	SUBCASE("Sort small dataset") {
		Ref<BitonicSort> sorter;
		sorter.instantiate();

		const uint32_t count = 256;
		Error err = sorter->initialize(rd, count);
		CHECK(err == OK);
		if (err != OK) {
			return;
		}

		// Create test data with random depths
		LocalVector<float> depths;
		LocalVector<uint32_t> indices;
		depths.resize(count);
		indices.resize(count);

		RandomNumberGenerator rng;
		rng.set_seed(42);

		for (uint32_t i = 0; i < count; i++) {
			depths[i] = rng.randf_range(0.0f, 100.0f);
			indices[i] = i;
		}

		// Create GPU buffers
		RID keys_buffer = create_storage_buffer(rd, depths);
		RID values_buffer = create_storage_buffer(rd, indices);

		// Sort on GPU
		err = sorter->sort(keys_buffer, values_buffer, count);
		CHECK(err == OK);

		// Read back results
		Vector<uint8_t> sorted_depths = rd->buffer_get_data(keys_buffer);

		// Verify sorting order
		const float *depth_ptr = (const float *)sorted_depths.ptr();
		for (uint32_t i = 1; i < count; i++) {
			CHECK(depth_ptr[i] >= depth_ptr[i - 1]);
		}

		// Clean up
		rd->free(keys_buffer);
		rd->free(values_buffer);
	}

	SUBCASE("Sort power-of-two sizes") {
		const uint32_t test_sizes[] = {128, 256, 512, 1024, 2048};

		for (uint32_t size : test_sizes) {
			Ref<BitonicSort> sorter;
			sorter.instantiate();

			Error err = sorter->initialize(rd, size);
			CHECK(err == OK);
			if (err != OK) {
				continue;
			}

			// Create reverse-sorted data (worst case)
			LocalVector<float> depths;
			LocalVector<uint32_t> indices;
			depths.resize(size);
			indices.resize(size);

			for (uint32_t i = 0; i < size; i++) {
				depths[i] = float(size - i); // Reverse order
				indices[i] = i;
			}

			// Create GPU buffers
			RID keys_buffer = create_storage_buffer(rd, depths);
			RID values_buffer = create_storage_buffer(rd, indices);

			// Sort
			err = sorter->sort(keys_buffer, values_buffer, size);
			CHECK(err == OK);

			// Verify
			Vector<uint8_t> sorted_depths = rd->buffer_get_data(keys_buffer);
			const float *depth_ptr = (const float *)sorted_depths.ptr();

			for (uint32_t i = 0; i < size; i++) {
				CHECK(Math::is_equal_approx(depth_ptr[i], float(i + 1)));
			}

			// Clean up
			rd->free(keys_buffer);
			rd->free(values_buffer);
		}
	}

	SUBCASE("Handle non-power-of-two sizes") {
		Ref<BitonicSort> sorter;
		sorter.instantiate();

		const uint32_t count = 1000; // Not a power of 2
		Error err = sorter->initialize(rd, count);
		CHECK(err == OK);
		if (err != OK) {
			return;
		}

		// BitonicSort handles non-power-of-two internally
		CHECK(sorter->supports_non_power_of_two());

		// Create test data
		LocalVector<float> depths;
		LocalVector<uint32_t> indices;
		depths.resize(count);
		indices.resize(count);

		RandomNumberGenerator rng;
		rng.set_seed(42);

		for (uint32_t i = 0; i < count; i++) {
			depths[i] = rng.randf_range(0.0f, 100.0f);
			indices[i] = i;
		}

		// Create GPU buffers
		RID keys_buffer = create_storage_buffer(rd, depths);
		RID values_buffer = create_storage_buffer(rd, indices);

		// Sort
		err = sorter->sort(keys_buffer, values_buffer, count);
		CHECK(err == OK);

		// Verify sorting
		Vector<uint8_t> sorted_depths = rd->buffer_get_data(keys_buffer);
		const float *depth_ptr = (const float *)sorted_depths.ptr();

		for (uint32_t i = 1; i < count; i++) {
			CHECK(depth_ptr[i] >= depth_ptr[i - 1]);
		}

		// Clean up
		rd->free(keys_buffer);
		rd->free(values_buffer);
	}
}

TEST_CASE("[GaussianSplatting][RequiresGPU] Radix sort factory honors 32-bit key layout") {
	struct GPUSortingConfigRestore {
		GPUSortingConfig previous_config;
		~GPUSortingConfigRestore() {
			g_gpu_sorting_config = previous_config;
		}
	} restore = { g_gpu_sorting_config };

	g_gpu_sorting_config.reset_to_defaults();
	g_gpu_sorting_config.radix_bits = 4;
	g_gpu_sorting_config.workgroup_size = GPUSortingConstants::DEFAULT_WORKGROUP_SIZE;

	RenderingDevice *rd = RenderingDevice::get_singleton();
	bool owns_local_device = false;
	if (!rd) {
		RenderingServer *rs = RenderingServer::get_singleton();
		if (rs) {
			rd = rs->create_local_rendering_device();
			owns_local_device = rd != nullptr;
		}
	}

	if (!rd) {
		MESSAGE("Skipping 32-bit radix sort factory test - no RenderingDevice available");
		return;
	}

	SortKeyConfig key_config;
	key_config.key_bits = 32;
	key_config.tile_bits = 16;
	key_config.depth_bits = 16;
	key_config.enable_tie_breaker = false;

	Ref<IGPUSorter> sorter = GPUSorterFactory::create_sorter(
			GPUSorterFactory::ALGORITHM_RADIX,
			rd,
			16,
			key_config);

	CHECK(sorter.is_valid());
	if (!sorter.is_valid()) {
		if (owns_local_device) {
			memdelete(rd);
		}
		return;
	}

	const uint32_t count = 5;
	LocalVector<uint32_t> keys;
	LocalVector<uint32_t> values;
	LocalVector<uint32_t> expected_keys;
	LocalVector<uint32_t> expected_values;
	keys.push_back(0x00030020u);
	keys.push_back(0x00010010u);
	keys.push_back(0x00020030u);
	keys.push_back(0x00000001u);
	keys.push_back(0x00010005u);
	values.push_back(0u);
	values.push_back(1u);
	values.push_back(2u);
	values.push_back(3u);
	values.push_back(4u);
	expected_keys.push_back(0x00000001u);
	expected_keys.push_back(0x00010005u);
	expected_keys.push_back(0x00010010u);
	expected_keys.push_back(0x00020030u);
	expected_keys.push_back(0x00030020u);
	expected_values.push_back(3u);
	expected_values.push_back(4u);
	expected_values.push_back(1u);
	expected_values.push_back(2u);
	expected_values.push_back(0u);

	RID keys_buffer = create_storage_buffer(rd, keys);
	rd->set_resource_name(keys_buffer, "GS_Test_Radix32_Keys");
	RID values_buffer = create_storage_buffer(rd, values);
	rd->set_resource_name(values_buffer, "GS_Test_Radix32_Values");

	Error err = sorter->sort(keys_buffer, values_buffer, count);
	CHECK(err == OK);

	if (err == OK) {
		Vector<uint8_t> keys_result = rd->buffer_get_data(keys_buffer, 0, count * sizeof(uint32_t));
		Vector<uint8_t> values_result = rd->buffer_get_data(values_buffer, 0, count * sizeof(uint32_t));
		const uint32_t *sorted_keys = (const uint32_t *)keys_result.ptr();
		const uint32_t *sorted_values = (const uint32_t *)values_result.ptr();

		for (uint32_t i = 0; i < count; i++) {
			CHECK(sorted_keys[i] == expected_keys[i]);
			CHECK(sorted_values[i] == expected_values[i]);
		}
	}

	SortingMetrics metrics = sorter->get_metrics();
	CHECK(metrics.last_key_bits == 32u);
	CHECK(metrics.last_radix_bits == 4u);
	CHECK(metrics.last_pass_count == 8u);
	CHECK(metrics.last_element_count == count);
	CHECK(metrics.last_element_count_known);
	CHECK_FALSE(metrics.last_sort_indirect);
	CHECK(metrics.last_sort_async);

	rd->free(keys_buffer);
	rd->free(values_buffer);
	sorter->shutdown();
	sorter.unref();
	if (owns_local_device) {
		memdelete(rd);
	}
}

TEST_CASE("[GaussianSplatting][RequiresGPU] GPU Sorting Performance") {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!rd) {
		RenderingServer *rs = RenderingServer::get_singleton();
		if (rs) {
			rd = rs->create_local_rendering_device();
		}
	}

	if (!rd) {
		MESSAGE("Skipping GPU sorting performance tests - no RenderingDevice available");
		return;
	}

	SUBCASE("Bitonic sort performance scaling") {
		const uint32_t test_sizes[] = {1024, 4096, 16384, 65536};

		for (uint32_t size : test_sizes) {
			Ref<BitonicSort> sorter;
			sorter.instantiate();

			Error err = sorter->initialize(rd, size);
			CHECK(err == OK);
			if (err != OK) {
				continue;
			}

			// Create random test data
			LocalVector<float> depths;
			LocalVector<uint32_t> indices;
			depths.resize(size);
			indices.resize(size);

			RandomNumberGenerator rng;
			rng.set_seed(42);

			for (uint32_t i = 0; i < size; i++) {
				depths[i] = rng.randf_range(0.0f, 1000.0f);
				indices[i] = i;
			}

			// Create GPU buffers
			RID keys_buffer = create_storage_buffer(rd, depths);
			RID values_buffer = create_storage_buffer(rd, indices);

			// Measure sort time
			uint64_t start = OS::get_singleton()->get_ticks_usec();

			err = sorter->sort(keys_buffer, values_buffer, size);
			CHECK(err == OK);

			uint64_t elapsed = OS::get_singleton()->get_ticks_usec() - start;
			float ms = elapsed / 1000.0f;

			// Performance expectations (conservative for CI)
			if (size <= 4096) {
				CHECK_MESSAGE(ms < 50.0f,
					vformat("Sorting %d elements took %.2fms, expected < 50ms", size, ms));
			} else if (size <= 16384) {
				CHECK_MESSAGE(ms < 100.0f,
					vformat("Sorting %d elements took %.2fms, expected < 100ms", size, ms));
			} else if (size <= 65536) {
				CHECK_MESSAGE(ms < 200.0f,
					vformat("Sorting %d elements took %.2fms, expected < 200ms", size, ms));
			}

			// Clean up
			rd->free(keys_buffer);
			rd->free(values_buffer);
		}
	}

	SUBCASE("Compare with CPU sorting") {
		const uint32_t count = 10000;

		// Generate test data
		LocalVector<float> depths;
		LocalVector<uint32_t> indices;
		depths.resize(count);
		indices.resize(count);

		RandomNumberGenerator rng;
		rng.set_seed(42);

		for (uint32_t i = 0; i < count; i++) {
			depths[i] = rng.randf_range(0.0f, 1000.0f);
			indices[i] = i;
		}

		// CPU sort timing
		LocalVector<float> cpu_depths = depths;
		LocalVector<uint32_t> cpu_indices = indices;

		uint64_t cpu_start = OS::get_singleton()->get_ticks_usec();

		// Sort indices by depth
		std::sort(cpu_indices.ptr(), cpu_indices.ptr() + count,
			[&cpu_depths](uint32_t a, uint32_t b) {
				return cpu_depths[a] < cpu_depths[b];
			});

		uint64_t cpu_elapsed = OS::get_singleton()->get_ticks_usec() - cpu_start;
		float cpu_ms = cpu_elapsed / 1000.0f;

		// GPU sort timing
		Ref<BitonicSort> sorter;
		sorter.instantiate();
		Error err = sorter->initialize(rd, count);
		CHECK(err == OK);
		if (err != OK) {
			return;
		}

		RID keys_buffer = create_storage_buffer(rd, depths);
		RID values_buffer = create_storage_buffer(rd, indices);

		uint64_t gpu_start = OS::get_singleton()->get_ticks_usec();

		err = sorter->sort(keys_buffer, values_buffer, count);
		CHECK(err == OK);

		uint64_t gpu_elapsed = OS::get_singleton()->get_ticks_usec() - gpu_start;
		float gpu_ms = gpu_elapsed / 1000.0f;

		// GPU should be reasonable compared to CPU (not always faster due to transfer overhead)
		CHECK_MESSAGE(gpu_ms < cpu_ms * 10.0f,
			vformat("GPU sort (%.2fms) should be within 10x of CPU sort (%.2fms)", gpu_ms, cpu_ms));

		// Clean up
		rd->free(keys_buffer);
		rd->free(values_buffer);
	}
}

// Regression guard for the 8-bit radix path: with 256 histogram/scatter bins, the per-bin shader
// loops must be STRIDED over WORKGROUP_SIZE. A naive `tid < RADIX_SIZE` guard only covers the first
// WORKGROUP_SIZE bins, silently corrupting the sort at workgroup_size 64/128. This test sorts keys
// whose bytes span HIGH digit values (>= 128) so those high bins are exercised, and checks the GPU
// output against a CPU reference at every allowed workgroup_size.
TEST_CASE("[GaussianSplatting][RequiresGPU] Radix sort 8-bit is correct at all workgroup sizes") {
	struct GPUSortingConfigRestore {
		GPUSortingConfig previous_config;
		~GPUSortingConfigRestore() { g_gpu_sorting_config = previous_config; }
	} restore = { g_gpu_sorting_config };

	RenderingDevice *rd = RenderingDevice::get_singleton();
	bool owns_local_device = false;
	if (!rd) {
		RenderingServer *rs = RenderingServer::get_singleton();
		if (rs) {
			rd = rs->create_local_rendering_device();
			owns_local_device = rd != nullptr;
		}
	}
	if (!rd) {
		MESSAGE("Skipping 8-bit radix sort test - no RenderingDevice available");
		return;
	}

	LocalVector<uint32_t> base_keys;
	base_keys.push_back(0xFF000001u);
	base_keys.push_back(0x80C04020u);
	base_keys.push_back(0x000000FFu);
	base_keys.push_back(0x01FF0080u);
	base_keys.push_back(0xC0C0C0C0u);
	base_keys.push_back(0x00018000u);
	base_keys.push_back(0xFFFFFFFFu);
	base_keys.push_back(0x00000000u);
	base_keys.push_back(0x7F80FE01u);
	base_keys.push_back(0x40404040u);
	const uint32_t count = base_keys.size();

	LocalVector<uint32_t> expected_keys = base_keys;
	expected_keys.sort(); // ascending reference order

	const uint32_t workgroup_sizes[] = { 64u, 128u, 256u, 512u };
	uint32_t verified = 0u;
	for (uint32_t wg : workgroup_sizes) {
		g_gpu_sorting_config.reset_to_defaults();
		g_gpu_sorting_config.radix_bits = 8;
		g_gpu_sorting_config.workgroup_size = wg;

		SortKeyConfig key_config;
		key_config.key_bits = 32;
		key_config.tile_bits = 16;
		key_config.depth_bits = 16;
		key_config.enable_tie_breaker = false;

		Ref<IGPUSorter> sorter = GPUSorterFactory::create_sorter(
				GPUSorterFactory::ALGORITHM_RADIX, rd, count, key_config);
		if (!sorter.is_valid()) {
			MESSAGE(vformat("Skipping 8-bit radix at workgroup_size=%d (unsupported on this GPU)", wg));
			continue;
		}

		LocalVector<uint32_t> keys = base_keys;
		LocalVector<uint32_t> values;
		values.resize(count);
		for (uint32_t i = 0; i < count; i++) {
			values[i] = i;
		}

		RID keys_buffer = create_storage_buffer(rd, keys);
		RID values_buffer = create_storage_buffer(rd, values);

		Error err = sorter->sort(keys_buffer, values_buffer, count);
		CHECK(err == OK);
		if (err == OK) {
			Vector<uint8_t> keys_result = rd->buffer_get_data(keys_buffer, 0, count * sizeof(uint32_t));
			Vector<uint8_t> values_result = rd->buffer_get_data(values_buffer, 0, count * sizeof(uint32_t));
			const uint32_t *sorted_keys = (const uint32_t *)keys_result.ptr();
			const uint32_t *sorted_values = (const uint32_t *)values_result.ptr();
			for (uint32_t i = 0; i < count; i++) {
				// (1) ascending order matches the CPU reference; (2) the carried value still points
				// back to the original key (proves the scatter moved key+value together).
				CHECK_MESSAGE(sorted_keys[i] == expected_keys[i],
						vformat("8-bit radix wg=%d pos %d: got 0x%08X expected 0x%08X", wg, i, sorted_keys[i], expected_keys[i]));
				// Compute the boolean separately: doctest cannot decompose a compound `a && b` inside CHECK.
				const bool value_maps_back = sorted_values[i] < count && base_keys[sorted_values[i]] == sorted_keys[i];
				CHECK_MESSAGE(value_maps_back,
						vformat("8-bit radix wg=%d pos %d: value %d does not map back to key 0x%08X", wg, i, sorted_values[i], sorted_keys[i]));
			}
			verified++;
		}

		SortingMetrics metrics = sorter->get_metrics();
		CHECK(metrics.last_radix_bits == 8u);
		CHECK(metrics.last_pass_count == 4u); // 32-bit key / 8-bit radix = 4 passes

		rd->free(keys_buffer);
		rd->free(values_buffer);
		sorter->shutdown();
		sorter.unref();
	}

	// If every workgroup size was rejected by the runtime probes (descriptor / shared-
	// memory / workgroup limits), an invalid sorter is the SUPPORTED production fallback —
	// the sort path keeps the 4-bit default — so skip rather than fail the RequiresGPU
	// suite on a low-capability GPU. When at least one variant was supported the per-
	// iteration CHECKs above already validated correctness.
	if (verified == 0u) {
		MESSAGE("Skipping 8-bit radix verification: no workgroup size is supported on this GPU");
	}

	if (owns_local_device) {
		memdelete(rd);
	}
}

} // namespace TestGaussianSplatting

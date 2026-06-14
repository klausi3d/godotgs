#pragma once

#include "test_macros.h"

#include "../renderer/gpu_buffer_manager.h"

#include "servers/rendering/rendering_device.h"
#include "servers/rendering_server.h"

#ifdef TESTS_ENABLED

namespace TestGaussianSplatting {

TEST_CASE("[GaussianSplatting][RequiresGPU] GPUBufferManager cleanup frees partial allocations before buffers_created") {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs) {
		return; // Skip in headless test mode (tag: [RequiresGPU])
	}

	RenderingDevice *rd = rs->create_local_rendering_device();
	bool owns_rd = true;
	if (!rd) {
		rd = rs->get_rendering_device();
		owns_rd = false;
	}
	if (!rd) {
		return; // Skip in headless test mode (tag: [RequiresGPU])
	}

	Ref<GPUBufferManager> buffer_manager;
	buffer_manager.instantiate();

	CHECK(buffer_manager->test_simulate_partial_allocation_cleanup(rd));

	if (owns_rd) {
		memdelete(rd);
	}
}

// S1: the instance pipeline never samples the GPUBufferManager gaussian_buffer (it binds
// the instance atlas), so production initializes with allocate_gaussian_buffer=false to
// reclaim it (~144 B x max_splats x BUFFER_COUNT). The live sort_key/sorted_indices buffers
// must always remain allocated (the sort pipeline adopts them as external buffers). The
// default (2-arg initialize) keeps the gaussian_buffer for the legacy painterly + test paths.
//
// This case locks the CONTRACT that RenderResourceOrchestrator::create_gpu_resources_safe()
// depends on: after a flag=false init the readiness check there keys off get_sort_key_buffer()
// + get_sorted_indices_buffer() (NOT get_current_read_buffer(), which now returns the skipped
// gaussian_buffer). The assertions below mirror exactly that getter set, so a regression that
// stopped allocating the sort buffers under flag=false would fail here. The orchestrator-level
// wiring itself is exercised by the real-game render gate (no sort fallback, byte-identical
// output); it is not unit-tested because create_gpu_resources_safe() needs the full device/
// shader/pipeline Dependencies and would only run under [RequiresGPU] anyway.
TEST_CASE("[GaussianSplatting][RequiresGPU] GPUBufferManager allocate_gaussian_buffer flag gates the dead buffer + memory") {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs) {
		return; // Skip in headless test mode (tag: [RequiresGPU])
	}
	RenderingDevice *rd = rs->create_local_rendering_device();
	bool owns_rd = true;
	if (!rd) {
		rd = rs->get_rendering_device();
		owns_rd = false;
	}
	if (!rd) {
		return; // Skip in headless test mode (tag: [RequiresGPU])
	}

	Ref<GPUBufferManager> bm;
	bm.instantiate();
	const uint32_t kCap = 4096;

	// Default path (flag defaults true): legacy/manual-upload path allocates everything.
	REQUIRE(bm->initialize(rd, kCap) == OK);
	CHECK(bm->get_current_read_buffer().is_valid()); // gaussian_buffer present
	CHECK(bm->get_current_read_handle().is_valid());
	CHECK(bm->get_sort_key_buffer().is_valid());
	CHECK(bm->get_sorted_indices_buffer().is_valid());
	const float mem_with = bm->get_memory_usage_mb();

	// Instance path (flag=false): gaussian_buffer skipped, live sort buffers kept.
	REQUIRE(bm->initialize(rd, kCap, /*allocate_gaussian_buffer=*/ false) == OK);
	CHECK_FALSE(bm->get_current_read_buffer().is_valid()); // gaussian_buffer skipped
	CHECK_FALSE(bm->get_current_read_handle().is_valid());
	CHECK(bm->get_sort_key_buffer().is_valid()); // live sort buffers still allocated
	CHECK(bm->get_sorted_indices_buffer().is_valid());
	CHECK(bm->get_sort_key_handle().is_valid()); // -> get_sort_external_buffer_state would be valid
	CHECK(bm->get_sorted_indices_handle().is_valid());
	const float mem_without = bm->get_memory_usage_mb();

	// The reclaimed amount matches sizeof(PackedGaussian) x kCap x BUFFER_COUNT(2).
	CHECK(mem_without < mem_with);
	const float expected_delta_mb = float(uint64_t(sizeof(PackedGaussian)) * kCap * 2u) / (1024.0f * 1024.0f);
	CHECK(mem_with - mem_without == doctest::Approx(expected_delta_mb).epsilon(0.02));

	// Release the manager (frees its buffers via ~GPUBufferManager) BEFORE deleting the
	// local device — otherwise the destructor would run cleanup on a freed device.
	bm.unref();
	if (owns_rd) {
		memdelete(rd);
	}
}

} // namespace TestGaussianSplatting

#endif // TESTS_ENABLED

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

} // namespace TestGaussianSplatting

#endif // TESTS_ENABLED

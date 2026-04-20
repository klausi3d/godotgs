#include "render_frame_context_manager.h"

#include "core/os/os.h"

#include <atomic>

RenderFrameContextManager::RenderFrameContextManager() {
	reset();
}

void RenderFrameContextManager::reset_frame_state() {
	frame_state.visible_splat_count.store(0, std::memory_order_release);
	frame_state.frame_counter = 0;
	frame_state.sort_time_ms = 0.0f;
	frame_state.render_time_ms = 0.0f;
	frame_state.animation_time_seconds = 0.0;
}

double RenderFrameContextManager::sample_render_animation_time_seconds() {
	// Lazy epoch: first call latches "now" as zero, later calls report
	// elapsed wall-clock seconds. std::atomic gives us thread-safe
	// initialization without a mutex; callers from the render thread and
	// any helper threads agree on the same epoch.
	static std::atomic<uint64_t> epoch_usec{0};
	uint64_t now_usec = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
	uint64_t expected = 0;
	if (epoch_usec.compare_exchange_strong(expected, now_usec, std::memory_order_acq_rel)) {
		return 0.0;
	}
	const uint64_t epoch = epoch_usec.load(std::memory_order_acquire);
	if (now_usec < epoch) {
		// Clock went backwards (can happen under manipulated or virtualized
		// clocks). Treat as "no time elapsed" rather than negative time so
		// shader phases don't regress.
		return 0.0;
	}
	return double(now_usec - epoch) * 1e-6;
}

void RenderFrameContextManager::reset_view_state_defaults() {
	view_state.last_camera_to_world_transform = Transform3D();
	view_state.last_camera_projection.set_perspective(60.0f, 16.0f / 9.0f, 0.1f, 1000.0f);
	view_state.last_camera_position = Vector3(0, 0, 5);
	view_state.manual_viewport_override = Size2i();
	view_state.manual_viewport_format_override = RD::DATA_FORMAT_MAX;
	view_state.active_viewport_color_format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	view_state.using_scene_data = false;
}

void RenderFrameContextManager::reset() {
	reset_frame_state();
	reset_view_state_defaults();
}

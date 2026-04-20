#ifndef GAUSSIAN_RENDER_FRAME_CONTEXT_MANAGER_H
#define GAUSSIAN_RENDER_FRAME_CONTEXT_MANAGER_H

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "servers/rendering/rendering_device.h"

#include <atomic>

class RenderFrameContextManager {
public:
	struct FrameState {
		std::atomic<uint32_t> visible_splat_count{0};
		uint32_t frame_counter = 0;
		float sort_time_ms = 0.0f;
		float render_time_ms = 0.0f;
		// Monotonic wall-clock seconds sampled once per render frame and
		// propagated to shader animation phase (wind, sphere effectors).
		// Previously the shader derived phase from `frame_counter / 60`,
		// which beat against script-side animations driven by real
		// `_process(delta)` on non-60Hz displays or under frame drops —
		// visible as jitter layered on the intended oscillation. Sampling
		// once per frame keeps the tile and depth stages in lockstep.
		double animation_time_seconds = 0.0;
	};

	// Returns monotonic seconds since the first call. Used by the render
	// pipeline to sample a single animation-time value per frame before
	// setting shader uniforms; see FrameState::animation_time_seconds.
	static double sample_render_animation_time_seconds();

	struct ViewState {
		Transform3D last_camera_to_world_transform;
		Projection last_camera_projection;
		Vector3 last_camera_position = Vector3(0, 0, 5);
		Size2i manual_viewport_override = Size2i();
		RD::DataFormat manual_viewport_format_override = RD::DATA_FORMAT_MAX;
		RD::DataFormat active_viewport_color_format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		bool using_scene_data = false;
	};

	RenderFrameContextManager();

	FrameState &get_frame_state() { return frame_state; }
	const FrameState &get_frame_state() const { return frame_state; }
	ViewState &get_view_state() { return view_state; }
	const ViewState &get_view_state() const { return view_state; }

	void reset_frame_state();
	void reset_view_state_defaults();
	void reset();

private:
	FrameState frame_state;
	ViewState view_state;
};

#endif

#pragma once

#include "test_macros.h"
#include "visual_compare.h"

#include "../interfaces/output_compositor.h"
#include "../interfaces/output_compositor_interfaces.h"

#include "core/io/image.h"
#include "core/math/math_funcs.h"
#include "core/object/ref_counted.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering_server.h"

#ifdef TESTS_ENABLED

namespace TestGaussianSplatting {
namespace CompositeHazardRepro {

// Build a 256x256 RGBA8 destination pre-filled with a horizontal gradient deliberately
// offset 3 pixels off the 8-pixel workgroup boundary. The historic black-blocks symptom
// manifested at 8-pixel grid lines because invocations in workgroup N read stale
// destination written by workgroup N-1 in unspecified order; an off-grid gradient makes
// any block-boundary artifact visible against a smooth reference baseline.
static Vector<uint8_t> _build_destination_gradient_256() {
	Vector<uint8_t> data;
	data.resize(256 * 256 * 4);
	uint8_t *w = data.ptrw();
	for (int y = 0; y < 256; y++) {
		for (int x = 0; x < 256; x++) {
			const int i = (y * 256 + x) * 4;
			const int gx = (x + 3) % 256;
			w[i + 0] = uint8_t(64 + (gx * 128) / 255);
			w[i + 1] = 128;
			w[i + 2] = 0;
			w[i + 3] = 255;
		}
	}
	return data;
}

// Build a 256x256 RGBA8 source: 25% area-equivalent centered circle (radius 72),
// premultiplied alpha (source.a = 0.5 inside, 0 outside).
static Vector<uint8_t> _build_source_premultiplied_circle_256() {
	Vector<uint8_t> data;
	data.resize(256 * 256 * 4);
	uint8_t *w = data.ptrw();
	const float cx = 128.0f;
	const float cy = 128.0f;
	const float r2 = 72.0f * 72.0f;
	for (int y = 0; y < 256; y++) {
		for (int x = 0; x < 256; x++) {
			const int i = (y * 256 + x) * 4;
			const float dx = float(x) - cx;
			const float dy = float(y) - cy;
			const bool inside = (dx * dx + dy * dy) < r2;
			w[i + 0] = inside ? 128 : 0;
			w[i + 1] = inside ? 128 : 0;
			w[i + 2] = inside ? 128 : 0;
			w[i + 3] = inside ? 128 : 0;
		}
	}
	return data;
}

} // namespace CompositeHazardRepro

TEST_CASE("[GaussianSplatting][RequiresGPU][HazardRepro] Compositor scratch-copy preserves destination outside source") {
	REQUIRE_GPU_DEVICE();
	// rd was declared by REQUIRE_GPU_DEVICE; it is the singleton or a local fallback.

	RD::TextureFormat source_format;
	source_format.width = 256;
	source_format.height = 256;
	source_format.depth = 1;
	source_format.array_layers = 1;
	source_format.mipmaps = 1;
	source_format.texture_type = RD::TEXTURE_TYPE_2D;
	source_format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	source_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_UPDATE_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;

	RD::TextureFormat destination_format;
	destination_format.width = 256;
	destination_format.height = 256;
	destination_format.depth = 1;
	destination_format.array_layers = 1;
	destination_format.mipmaps = 1;
	destination_format.texture_type = RD::TEXTURE_TYPE_2D;
	destination_format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	destination_format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_UPDATE_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;

	RID source_tex = rd->texture_create(source_format, RD::TextureView());
	RID destination_tex = rd->texture_create(destination_format, RD::TextureView());
	REQUIRE(source_tex.is_valid());
	REQUIRE(destination_tex.is_valid());
	rd->set_resource_name(source_tex, "GS_HazardRepro_Source");
	rd->set_resource_name(destination_tex, "GS_HazardRepro_Destination");

	// copy_to_render_target only enters _copy_final_output_compute (the scratch-copy path)
	// when depth_test_enabled is true; composite-without-depth falls back to copy_to_fb_rect
	// which requires CopyEffects (not available in the headless harness). Bind dummy depth
	// textures so the compute path runs. The internal depth comparison is effectively a
	// no-op because both depths are uniform.
	RD::TextureFormat depth_format;
	depth_format.width = 256;
	depth_format.height = 256;
	depth_format.depth = 1;
	depth_format.array_layers = 1;
	depth_format.mipmaps = 1;
	depth_format.texture_type = RD::TEXTURE_TYPE_2D;
	depth_format.format = RD::DATA_FORMAT_D32_SFLOAT;
	depth_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	RID source_depth = rd->texture_create(depth_format, RD::TextureView());
	RID destination_depth = rd->texture_create(depth_format, RD::TextureView());
	REQUIRE(source_depth.is_valid());
	REQUIRE(destination_depth.is_valid());
	rd->set_resource_name(source_depth, "GS_HazardRepro_SourceDepth");
	rd->set_resource_name(destination_depth, "GS_HazardRepro_DestinationDepth");

	Vector<uint8_t> destination_data = CompositeHazardRepro::_build_destination_gradient_256();
	Vector<uint8_t> source_data = CompositeHazardRepro::_build_source_premultiplied_circle_256();
	REQUIRE(rd->texture_update(destination_tex, 0, destination_data) == OK);
	REQUIRE(rd->texture_update(source_tex, 0, source_data) == OK);

	Ref<OutputCompositor> compositor;
	compositor.instantiate();
	REQUIRE(compositor->initialize(rd) == OK);

	OutputCopyParams params;
	params.source_texture = source_tex;
	params.source_depth = source_depth;
	params.destination_texture = destination_tex;
	params.destination_depth = destination_depth;
	params.viewport_size = Size2i(256, 256);
	params.composite_with_destination = true;
	params.source_is_premultiplied = true;
	params.depth_test_enabled = true;
	params.z_near = 0.05f;
	params.z_far = 4000.0f;
	params.depth_linearize_mul = 1.0f;
	params.depth_linearize_add = 1.0f;
	params.depth_epsilon = 0.01f;

	OutputCopyResult result = compositor->copy_to_render_target(params);
	if (!result.success) {
		print_line(vformat("[HazardRepro] copy_to_render_target failed: %s", result.error));
	}
	CHECK(result.success);

	const OutputCompositor::LastCompositeStats stats = compositor->get_last_composite_stats();
	CHECK(stats.valid);
	CHECK(stats.composite_with_destination);
	CHECK(stats.scratch_used);
	CHECK_FALSE(stats.fallback_due_to_missing_copy_from);
	CHECK_EQ(stats.binding_count, 5u);

	String visual_reason;
	const bool visual_ok = TestGaussianSplatting::VisualCompare::capture_and_compare(
			rd, destination_tex, "composite_hazard_256x256.png", 1.0, 45.0, &visual_reason);
	if (!visual_ok) {
		print_line(vformat("[HazardRepro] visual gate: %s", visual_reason));
	}
	CHECK(visual_ok);

	// Second invocation must reuse the scratch entry (same format and extent).
	Vector<uint8_t> destination_reset = CompositeHazardRepro::_build_destination_gradient_256();
	REQUIRE(rd->texture_update(destination_tex, 0, destination_reset) == OK);
	OutputCopyResult result_second = compositor->copy_to_render_target(params);
	CHECK(result_second.success);
	const OutputCompositor::LastCompositeStats stats_second = compositor->get_last_composite_stats();
	CHECK(stats_second.scratch_used);
	CHECK(stats_second.scratch_reused);

	compositor->shutdown();
	rd->free(source_tex);
	rd->free(destination_tex);
	rd->free(source_depth);
	rd->free(destination_depth);
}

} // namespace TestGaussianSplatting

#endif // TESTS_ENABLED

#pragma once

#include "test_macros.h"

#include "../renderer/gaussian_splat_renderer.h"

TEST_CASE("[GaussianSplatting][Renderer] scoped shadow pass guard restores renderer state after failure scope") {
	Ref<GaussianSplatRenderer> renderer;
	renderer.instantiate();
	REQUIRE(renderer.is_valid());

	CHECK(renderer->test_shadow_pass_guard_restores_after_scope());
}

TEST_CASE("[GaussianSplatting][Renderer] shadow pass reports invalid atlas as shadow-specific failure") {
	Ref<GaussianSplatRenderer> renderer;
	renderer.instantiate();
	REQUIRE(renderer.is_valid());

	const Transform3D light_transform;
	Projection light_projection;
	light_projection.set_perspective(45.0, 1.0, 0.1, 100.0);

	const bool rendered = renderer->render_shadow_depth_map(light_projection, light_transform,
			Rect2i(0, 0, 0, 64), RID(), false);
	CHECK_FALSE(rendered);

	const GaussianSplatRenderer::ShadowRenderResult &result = renderer->get_last_shadow_render_result();
	CHECK_EQ(result.pass_kind, GaussianSplatRenderer::RenderPassKind::SHADOW_MAP);
	CHECK_FALSE(result.success);
	CHECK_EQ(result.failure_reason, GaussianSplatRenderer::ShadowRenderFailureReason::INVALID_ATLAS_RECT);
	CHECK_EQ(result.route_label, String("SHADOW_FAIL_INVALID_ATLAS_RECT"));
}

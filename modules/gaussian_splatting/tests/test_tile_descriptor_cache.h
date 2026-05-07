/**************************************************************************/
/*  test_tile_descriptor_cache.h                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "tests/test_macros.h"

#include "../renderer/tile_renderer.h"

namespace TestGaussianSplatting {

#if defined(TESTS_ENABLED)

static TileRenderer::RenderParams make_instance_pipeline_params_for_descriptor_cache_test(uint64_t p_base) {
	TileRenderer::RenderParams params;
	params.instance_buffer = RID::from_uint64(p_base + 1u);
	params.instance_grading_buffer = RID::from_uint64(p_base + 2u);
	params.splat_ref_buffer = RID::from_uint64(p_base + 3u);
	params.instance_indirect_count_buffer = RID::from_uint64(p_base + 4u);
	params.instance_indirect_dispatch_buffer = RID::from_uint64(p_base + 5u);
	return params;
}

TEST_CASE("[GaussianSplatting][TileRenderer] Instance pipeline RID drift invalidates descriptor generation") {
	TileRenderer::InstancePipelineBindings bindings;
	TileRenderer::RenderParams params;
	uint64_t descriptor_generation = 0;

	CHECK_FALSE(TileRenderer::_test_apply_instance_pipeline_bindings(bindings, descriptor_generation, params));
	CHECK(descriptor_generation == 0u);

	params = make_instance_pipeline_params_for_descriptor_cache_test(0x24700u);
	CHECK(TileRenderer::_test_apply_instance_pipeline_bindings(bindings, descriptor_generation, params));
	CHECK(descriptor_generation == 1u);

	CHECK_FALSE(TileRenderer::_test_apply_instance_pipeline_bindings(bindings, descriptor_generation, params));
	CHECK(descriptor_generation == 1u);

	params.instance_buffer = RID::from_uint64(0x24801u);
	CHECK(TileRenderer::_test_apply_instance_pipeline_bindings(bindings, descriptor_generation, params));
	CHECK(descriptor_generation == 2u);

	params.instance_grading_buffer = RID::from_uint64(0x24802u);
	CHECK(TileRenderer::_test_apply_instance_pipeline_bindings(bindings, descriptor_generation, params));
	CHECK(descriptor_generation == 3u);

	params.splat_ref_buffer = RID::from_uint64(0x24803u);
	CHECK(TileRenderer::_test_apply_instance_pipeline_bindings(bindings, descriptor_generation, params));
	CHECK(descriptor_generation == 4u);

	params.instance_indirect_count_buffer = RID::from_uint64(0x24804u);
	CHECK(TileRenderer::_test_apply_instance_pipeline_bindings(bindings, descriptor_generation, params));
	CHECK(descriptor_generation == 5u);

	params.instance_indirect_dispatch_buffer = RID::from_uint64(0x24805u);
	CHECK(TileRenderer::_test_apply_instance_pipeline_bindings(bindings, descriptor_generation, params));
	CHECK(descriptor_generation == 6u);

	params.quantization_buffer = RID::from_uint64(0x24806u);
	CHECK(TileRenderer::_test_apply_instance_pipeline_bindings(bindings, descriptor_generation, params));
	CHECK(descriptor_generation == 7u);

	params.chunk_meta_buffer = RID::from_uint64(0x24807u);
	CHECK(TileRenderer::_test_apply_instance_pipeline_bindings(bindings, descriptor_generation, params));
	CHECK(descriptor_generation == 8u);

	params.chunk_meta_buffer = RID::from_uint64(0x24808u);
	CHECK(TileRenderer::_test_apply_instance_pipeline_bindings(bindings, descriptor_generation, params));
	CHECK(descriptor_generation == 9u);

	params.quantization_buffer = RID();
	CHECK(TileRenderer::_test_apply_instance_pipeline_bindings(bindings, descriptor_generation, params));
	CHECK(descriptor_generation == 10u);
}

#endif // TESTS_ENABLED

} // namespace TestGaussianSplatting

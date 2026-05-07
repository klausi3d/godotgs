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
	Vector<TileRenderer::RenderParams> sequence;

	TileRenderer::RenderParams params;
	sequence.push_back(params);

	params = make_instance_pipeline_params_for_descriptor_cache_test(0x24700u);
	sequence.push_back(params);

	sequence.push_back(params);

	params.instance_buffer = RID::from_uint64(0x24801u);
	sequence.push_back(params);

	params.instance_grading_buffer = RID::from_uint64(0x24802u);
	sequence.push_back(params);

	params.splat_ref_buffer = RID::from_uint64(0x24803u);
	sequence.push_back(params);

	params.instance_indirect_count_buffer = RID::from_uint64(0x24804u);
	sequence.push_back(params);

	params.instance_indirect_dispatch_buffer = RID::from_uint64(0x24805u);
	sequence.push_back(params);

	params.quantization_buffer = RID::from_uint64(0x24806u);
	sequence.push_back(params);

	params.chunk_meta_buffer = RID::from_uint64(0x24807u);
	sequence.push_back(params);

	params.chunk_meta_buffer = RID::from_uint64(0x24808u);
	sequence.push_back(params);

	params.quantization_buffer = RID();
	sequence.push_back(params);

	const Vector<uint64_t> trace = TileRenderer::_test_instance_pipeline_binding_generation_trace(sequence);
	REQUIRE(trace.size() == 12);
	CHECK(trace[0] == 0u);
	CHECK(trace[1] == 1u);
	CHECK(trace[2] == 1u);
	CHECK(trace[3] == 2u);
	CHECK(trace[4] == 3u);
	CHECK(trace[5] == 4u);
	CHECK(trace[6] == 5u);
	CHECK(trace[7] == 6u);
	CHECK(trace[8] == 7u);
	CHECK(trace[9] == 8u);
	CHECK(trace[10] == 9u);
	CHECK(trace[11] == 10u);
}

#endif // TESTS_ENABLED

} // namespace TestGaussianSplatting

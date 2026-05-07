#pragma once

#include "test_macros.h"

#include "../renderer/tile_lighting_abi.h"

#ifdef TESTS_ENABLED

TEST_CASE("[GaussianSplatting][TileLightingABI] resolve set constants match shader bridge contract") {
	using ABI = GaussianSplatting::TileLightingSetABI;

	CHECK(ABI::SET == 2u);
	CHECK(ABI::BINDING_SCENE_DATA == 0u);
	CHECK(ABI::BINDING_DIRECTIONAL_LIGHTS == 1u);
	CHECK(ABI::BINDING_OMNI_LIGHTS == 2u);
	CHECK(ABI::BINDING_SPOT_LIGHTS == 3u);
	CHECK(ABI::BINDING_REFLECTIONS == 4u);
	CHECK(ABI::BINDING_DECAL_ATLAS == 5u);
	CHECK(ABI::BINDING_REFLECTION_ATLAS == 6u);
	CHECK(ABI::BINDING_LIGHT_PROJECTOR_SAMPLER == 7u);
	CHECK(ABI::BINDING_DEFAULT_SAMPLER_LINEAR_MIPMAPS_CLAMP == 8u);
	CHECK(ABI::BINDING_CLUSTER_BUFFER == 9u);
	CHECK(ABI::BINDING_SHADOW_SAMPLER == 10u);
	CHECK(ABI::BINDING_SHADOW_ATLAS == 11u);
	CHECK(ABI::BINDING_DIRECTIONAL_SHADOW_ATLAS == 12u);
	CHECK(ABI::BINDING_SAMPLER_LINEAR_CLAMP == 13u);

	CHECK(ABI::MIN_SCENE_DATA_UNIFORM_BYTES >= 8192u);
	CHECK(ABI::MIN_DIRECTIONAL_LIGHT_UNIFORM_BYTES >= 2048u);
	CHECK(ABI::MIN_LIGHT_STORAGE_BYTES >= 32u * 192u);
	CHECK(ABI::MIN_REFLECTION_STORAGE_BYTES >= 1024u);
	CHECK(ABI::MIN_CLUSTER_STORAGE_BYTES >= 1024u);
}

TEST_CASE("[GaussianSplatting][TileLightingABI] cluster config matches Godot packed layout") {
	using ABI = GaussianSplatting::TileLightingSetABI;

	const GaussianSplatting::TileLightingClusterABIConfig config =
			ABI::compute_cluster_config(Vector2i(1920, 1080), 32u, 512u, true);

	CHECK(config.enabled);
	CHECK(config.cluster_shift == 5u);
	CHECK(config.cluster_width == 60u);
	CHECK(config.max_cluster_element_count_div_32 == 16u);
	CHECK(config.cluster_type_size == 60u * 34u * (16u + 32u));
	CHECK(config.required_storage_bytes == uint64_t(config.cluster_type_size) *
					ABI::CLUSTER_ELEMENT_TYPES * ABI::CLUSTER_WORD_BYTES);
}

TEST_CASE("[GaussianSplatting][TileLightingABI] cluster config rejects incompatible metadata") {
	using ABI = GaussianSplatting::TileLightingSetABI;

	CHECK(!ABI::compute_cluster_config(Vector2i(1920, 1080), 30u, 512u, true).enabled);
	CHECK(!ABI::compute_cluster_config(Vector2i(1920, 1080), 32u, 500u, true).enabled);
	CHECK(!ABI::compute_cluster_config(Vector2i(0, 1080), 32u, 512u, true).enabled);
	CHECK(!ABI::compute_cluster_config(Vector2i(1920, 1080), 32u, 512u, false).enabled);
}

#endif // TESTS_ENABLED

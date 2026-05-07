#pragma once

#include "core/math/vector2i.h"

#include <cstdint>

namespace GaussianSplatting {

struct TileLightingClusterABIConfig {
	bool enabled = false;
	uint32_t cluster_shift = 0;
	uint32_t cluster_width = 0;
	uint32_t cluster_type_size = 0;
	uint32_t max_cluster_element_count_div_32 = 0;
	uint64_t required_storage_bytes = 0;
	const char *disabled_reason = nullptr;
};

struct TileLightingSetABI {
	static constexpr uint32_t SET = 2;

	static constexpr uint32_t BINDING_SCENE_DATA = 0;
	static constexpr uint32_t BINDING_DIRECTIONAL_LIGHTS = 1;
	static constexpr uint32_t BINDING_OMNI_LIGHTS = 2;
	static constexpr uint32_t BINDING_SPOT_LIGHTS = 3;
	static constexpr uint32_t BINDING_REFLECTIONS = 4;
	static constexpr uint32_t BINDING_DECAL_ATLAS = 5;
	static constexpr uint32_t BINDING_REFLECTION_ATLAS = 6;
	static constexpr uint32_t BINDING_LIGHT_PROJECTOR_SAMPLER = 7;
	static constexpr uint32_t BINDING_DEFAULT_SAMPLER_LINEAR_MIPMAPS_CLAMP = 8;
	static constexpr uint32_t BINDING_CLUSTER_BUFFER = 9;
	static constexpr uint32_t BINDING_SHADOW_SAMPLER = 10;
	static constexpr uint32_t BINDING_SHADOW_ATLAS = 11;
	static constexpr uint32_t BINDING_DIRECTIONAL_SHADOW_ATLAS = 12;
	static constexpr uint32_t BINDING_SAMPLER_LINEAR_CLAMP = 13;

	// Conservative shader-layout minimums for the zero fallback buffers bound by
	// create_lighting_uniform_set(). They intentionally stay at or above the
	// historical fallback sizes and the Godot 4.x shader structs used by
	// gs_lighting_bridge.glsl:
	// - SceneDataBlock is two std140 SceneData structs, about 5.7 KiB with MAX_VIEWS=2.
	// - DirectionalLightData is a std140 array entry, about 464 bytes.
	// - LightData is 192 bytes in std430; 32 entries cover offline shader variants.
	// - ReflectionData is about 128 bytes in std430.
	// - ClusterBuffer needs at least one 32-slice record for omni and spot masks.
	static constexpr uint32_t MIN_SCENE_DATA_UNIFORM_BYTES = 8192;
	static constexpr uint32_t MIN_DIRECTIONAL_LIGHT_UNIFORM_BYTES = 2048;
	static constexpr uint32_t MIN_LIGHT_STORAGE_BYTES = 32 * 192;
	static constexpr uint32_t MIN_REFLECTION_STORAGE_BYTES = 1024;
	static constexpr uint32_t MIN_CLUSTER_STORAGE_BYTES = 1024;

	static constexpr uint32_t CLUSTER_ELEMENT_TYPES = 2;
	static constexpr uint32_t CLUSTER_Z_SLICES = 32;
	static constexpr uint32_t CLUSTER_WORD_BYTES = 4;

	static constexpr bool is_power_of_two(uint32_t p_value) {
		return p_value != 0 && (p_value & (p_value - 1)) == 0;
	}

	static constexpr uint32_t shift_from_power_of_two(uint32_t p_value) {
		uint32_t shift = 0;
		while ((uint32_t(1) << shift) < p_value && shift < 31) {
			shift++;
		}
		return shift;
	}

	static uint64_t cluster_storage_bytes(uint32_t p_cluster_width, uint32_t p_cluster_height, uint32_t p_max_cluster_element_count_div_32) {
		return uint64_t(p_cluster_width) * uint64_t(p_cluster_height) *
				uint64_t(p_max_cluster_element_count_div_32 + CLUSTER_Z_SLICES) *
				uint64_t(CLUSTER_ELEMENT_TYPES) * uint64_t(CLUSTER_WORD_BYTES);
	}

	static TileLightingClusterABIConfig compute_cluster_config(const Vector2i &p_viewport_size,
			uint32_t p_cluster_size, uint32_t p_cluster_max_elements, bool p_cluster_buffer_valid) {
		TileLightingClusterABIConfig config;
		if (!p_cluster_buffer_valid) {
			config.disabled_reason = "missing cluster_buffer RID";
			return config;
		}
		if (p_viewport_size.x <= 0 || p_viewport_size.y <= 0) {
			config.disabled_reason = "non-positive viewport size";
			return config;
		}
		if (!is_power_of_two(p_cluster_size)) {
			config.disabled_reason = "cluster_size must be a non-zero power of two";
			return config;
		}
		if (p_cluster_max_elements < 32 || (p_cluster_max_elements % 32) != 0) {
			config.disabled_reason = "cluster_max_elements must be a multiple of 32";
			return config;
		}

		const uint32_t width = (uint32_t(p_viewport_size.x) + p_cluster_size - 1) / p_cluster_size;
		const uint32_t height = (uint32_t(p_viewport_size.y) + p_cluster_size - 1) / p_cluster_size;
		const uint32_t max_div_32 = p_cluster_max_elements / 32;
		const uint64_t type_size = uint64_t(width) * uint64_t(height) * uint64_t(max_div_32 + CLUSTER_Z_SLICES);
		if (type_size > UINT32_MAX) {
			config.disabled_reason = "cluster buffer layout exceeds 32-bit shader offsets";
			return config;
		}

		config.enabled = true;
		config.cluster_shift = shift_from_power_of_two(p_cluster_size);
		config.cluster_width = width;
		config.cluster_type_size = uint32_t(type_size);
		config.max_cluster_element_count_div_32 = max_div_32;
		config.required_storage_bytes = cluster_storage_bytes(width, height, max_div_32);
		return config;
	}
};

static_assert(TileLightingSetABI::MIN_LIGHT_STORAGE_BYTES >= 1024, "Light fallback must preserve the legacy minimum");
static_assert(TileLightingSetABI::MIN_REFLECTION_STORAGE_BYTES >= 1024, "Reflection fallback must preserve the legacy minimum");
static_assert(TileLightingSetABI::MIN_CLUSTER_STORAGE_BYTES >= 1024, "Cluster fallback must preserve the legacy minimum");

} // namespace GaussianSplatting

#ifndef GS_LIGHTING_BRIDGE_GLSL
#define GS_LIGHTING_BRIDGE_GLSL

#ifndef MAX_VIEWS
#define MAX_VIEWS 2
#endif

#ifndef M_PI
#define M_PI 3.14159265359
#endif

#ifndef MAX_ROUGHNESS_LOD
#define MAX_ROUGHNESS_LOD 5.0
#endif

#ifndef MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS
#define MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS 1
#endif

#include "../../../../servers/rendering/renderer_rd/shaders/half_inc.glsl"
#include "../../../../servers/rendering/renderer_rd/shaders/scene_data_inc.glsl"
#include "../../../../servers/rendering/renderer_rd/shaders/light_data_inc.glsl"

// Tile resolve lighting ABI, set=2.
// Host contract: TileResolveStage::create_lighting_uniform_set() must bind the
// exact resources below. Binding numbers and conservative fallback buffer
// minimums live in renderer/tile_lighting_abi.h so the C++ fallback path and
// this shader bridge stay in lockstep without depending on private Godot
// renderer structs.
//
// 0  std140 SceneDataBlock        SceneData data + SceneData prev_data
// 1  std140 DirectionalLights     DirectionalLightData[]
// 2  std430 OmniLights            LightData[]
// 3  std430 SpotLights            LightData[]
// 4  std430 ReflectionProbeData   ReflectionData[]
// 5  texture2D decal_atlas_srgb
// 6  textureCubeArray reflection_atlas
// 7  sampler light_projector_sampler
// 8  sampler DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP
// 9  std430 ClusterBuffer         uint[] in Godot ClusterBuilderRD layout
// 10 sampler shadow_sampler
// 11 texture2D shadow_atlas
// 12 texture2D directional_shadow_atlas
// 13 sampler SAMPLER_LINEAR_CLAMP

// ============================================================================
// Compute shader compatibility shim
// ============================================================================
// The Godot scene_forward_lights_inc.glsl uses gl_FragCoord for shadow sampling
// randomization. In compute shaders, gl_FragCoord doesn't exist, so we provide
// a substitute. Soft shadows are gated off here via sc_use_light_soft_shadows()
// returning false; that is the real switch, so the gl_FragCoord-using soft-shadow
// code paths are never executed at runtime. Note that the sample-count specializers
// sc_soft_shadow_samples()/sc_directional_soft_shadow_samples() deliberately return
// 4u (forward-prep for when soft shadows are enabled), so the substitute still needs
// to parse and compile.
//
// Compute shaders should define GS_COMPUTE_SHADER before including this file
// and can optionally set gs_frag_coord_substitute to a meaningful value if
// soft shadows are ever enabled in the future.
// ============================================================================
#ifdef GS_COMPUTE_SHADER
// Provide a substitute for gl_FragCoord in compute shaders.
// This is used by shadow sampling for randomization; since soft shadows are
// disabled, the actual value doesn't matter - it just needs to compile.
vec4 gs_frag_coord_substitute = vec4(0.0);
#define gl_FragCoord gs_frag_coord_substitute
#endif // GS_COMPUTE_SHADER

// Compute-shader fallback: projector lights are unavailable.
bool sc_use_light_projector() {
	return false;
}

// Compute-shader fallback: soft shadows are unavailable.
bool sc_use_light_soft_shadows() {
	return false;
}

// Compute-shader fallback: projector mipmaps are unavailable.
bool sc_projector_use_mipmaps() {
	return false;
}

// Match Godot's SHADOW_QUALITY_SOFT_LOW default (4 taps) so gaussian
// splat shadows have comparable quality to mesh shadows. Without PCF,
// each pixel gets a binary 0/1 shadow from a single compare, producing
// harsh per-pixel outlines.
uint sc_soft_shadow_samples() {
	return 4u;
}

uint sc_penumbra_shadow_samples() {
	return 0u;
}

uint sc_directional_soft_shadow_samples() {
	return 4u;
}

uint sc_directional_penumbra_shadow_samples() {
	return 0u;
}

// Compute-shader fallback: keep luminance neutral in the bridge path.
float sc_luminance_multiplier() {
	return 1.0;
}

layout(set = 2, binding = 0, std140) uniform SceneDataBlock {
	SceneData data;
	SceneData prev_data;
}
scene_data_block;

layout(set = 2, binding = 1, std140) uniform DirectionalLights {
	DirectionalLightData data[MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS];
}
directional_lights;

layout(set = 2, binding = 2, std430) restrict readonly buffer OmniLights {
	LightData data[];
}
omni_lights;

layout(set = 2, binding = 3, std430) restrict readonly buffer SpotLights {
	LightData data[];
}
spot_lights;

layout(set = 2, binding = 4, std430) restrict readonly buffer ReflectionProbeData {
	ReflectionData data[];
}
reflections;

layout(set = 2, binding = 9, std430) restrict readonly buffer ClusterBuffer {
	uint data[];
}
cluster_buffer;

layout(set = 2, binding = 5) uniform texture2D decal_atlas_srgb;
layout(set = 2, binding = 6) uniform textureCubeArray reflection_atlas;
layout(set = 2, binding = 7) uniform sampler light_projector_sampler;
layout(set = 2, binding = 8) uniform sampler DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP;
layout(set = 2, binding = 10) uniform sampler shadow_sampler;
layout(set = 2, binding = 11) uniform texture2D shadow_atlas;
layout(set = 2, binding = 12) uniform texture2D directional_shadow_atlas;
layout(set = 2, binding = 13) uniform sampler SAMPLER_LINEAR_CLAMP;

#include "../../../../servers/rendering/renderer_rd/shaders/scene_forward_lights_inc.glsl"

#endif // GS_LIGHTING_BRIDGE_GLSL

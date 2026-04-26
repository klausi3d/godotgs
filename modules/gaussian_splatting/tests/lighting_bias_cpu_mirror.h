#pragma once

// CPU-side mirror of the receiver-bias helper in
// `modules/gaussian_splatting/shaders/includes/gs_lighting_common.glsl`
// (`gs_compute_receiver_bias`). The shader is the production code path;
// this header exists so doctests can exercise the same math without a
// GPU. Any edit to the shader helper must be reflected here (and
// vice-versa), or the tests in `test_lighting_bias.h` will drift from
// the shader and become misleading.
//
// Contract (mirrored from gs_render_params.glsl:77-80 and
// gaussian_gpu_layout.h:456-459):
//   shadow_bias_config.x = receiver_bias_scale (per-splat radius multiplier)
//   shadow_bias_config.y = receiver_bias_min
//   shadow_bias_config.z = receiver_bias_max (0.0 disables the clamp)
//
// Resolve paths without a per-pixel representative radius pass 0.0,
// which intentionally collapses to the .y minimum (clamped by .z).

#include "core/math/math_funcs.h"

namespace GaussianSplatting {
namespace LightingBiasCPUMirror {

// shadow_bias_config laid out exactly like the shader's vec4: x=scale,
// y=min, z=max (>0 enables clamp), w=reserved.
struct ShadowBiasConfig {
	float scale;
	float min_bias;
	float max_bias;
};

inline float gs_compute_receiver_bias(const ShadowBiasConfig &cfg, float representative_radius) {
	float bias = MAX(cfg.min_bias, cfg.scale * representative_radius);
	if (cfg.max_bias > 0.0f) {
		bias = MIN(bias, cfg.max_bias);
	}
	return bias;
}

} // namespace LightingBiasCPUMirror
} // namespace GaussianSplatting

#pragma once

// CPU-side mirror of the sphere-effector / wind deformation math in
// `modules/gaussian_splatting/shaders/includes/gs_deformation.glsl`. The
// shader is the production code path; this header exists so doctests can
// exercise the same math without a GPU. Any edit to the shader must be
// reflected here (and vice-versa), or the tests in
// `test_deformation_math.h` will drift from the shader and become
// misleading.
//
// Keep this header free of Godot dependencies beyond what the shader
// semantics require — the point is a tight, reviewable reference.

#include "core/math/math_funcs.h"
#include "core/math/vector3.h"

#include <cstdint>

namespace GaussianSplatting {
namespace DeformationCPUMirror {

// Mirror of GS_INSTANCE_WIND_MODE_* from gs_instance_layout.glsl.
enum WindMode : uint32_t {
	WIND_MODE_INHERIT = 0u,
	WIND_MODE_FORCE_DISABLED = 1u,
	WIND_MODE_FORCE_ENABLED = 2u,
};

// Mirror of the per-instance wind-strength fallback path in
// gs_apply_wind_deformation() after the #3 fix (wind fallback). When the
// project-level wind_strength is zero but an instance forces wind on via
// its own override, the shader substitutes a unit base so the instance's
// multiplicative contributions (intensity, frequency) are effective.
inline float effective_wind_strength(float global_strength, WindMode mode) {
	const float base = MAX(global_strength, 0.0f);
	if (mode == WIND_MODE_FORCE_ENABLED && base <= 0.0f) {
		return 1.0f;
	}
	return base;
}

inline float effective_wind_temporal_frequency(float global_frequency, WindMode mode) {
	const float base = MAX(global_frequency, 0.0f);
	if (mode == WIND_MODE_FORCE_ENABLED && base <= 0.0f) {
		return 1.0f;
	}
	return base;
}

// Mirror of gs_compute_sphere_effector_weight() after #1 (sphere position
// sway). Returns a signed temporal factor in [-1, 1] so splats oscillate
// through rest, and couples the phase to a low-frequency spatial term so
// neighbouring splats stay in phase — no per-splat random jitter. Callers
// use the returned factor multiplied by the spatial influence (returned as
// `spatial_influence`) to drive position deformation.
struct SphereAnimSample {
	float spatial_influence; // 0..1, steady spatial falloff envelope
	float anim_factor; // -1..1, signed temporal pulse used by position
};

inline SphereAnimSample sphere_effector_anim_sample(
		const Vector3 &world_position,
		const Vector3 &effector_center,
		float radius,
		float falloff,
		float frequency,
		float time_seconds) {
	SphereAnimSample out = { 0.0f, 0.0f };
	if (radius <= 1e-6f) {
		return out;
	}
	const Vector3 delta = world_position - effector_center;
	const float distance_to_center = delta.length();
	if (distance_to_center >= radius) {
		return out;
	}
	const float normalized = CLAMP(1.0f - (distance_to_center / radius), 0.0f, 1.0f);
	const float falloff_safe = MAX(falloff, 0.001f);
	out.spatial_influence = Math::pow(normalized, falloff_safe);
	const float freq_safe = frequency > 0.0f ? frequency : 2.0f;
	const float spatial_coeff = 1.0f / MAX(radius, 1e-3f);
	const float spatial_phase = delta.dot(Vector3(spatial_coeff, spatial_coeff, spatial_coeff));
	const float anim_phase = time_seconds * freq_safe * 6.28318530718f + spatial_phase;
	out.anim_factor = Math::sin(anim_phase);
	return out;
}

} // namespace DeformationCPUMirror
} // namespace GaussianSplatting

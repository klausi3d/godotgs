#pragma once

#include "test_macros.h"
#include "lighting_bias_cpu_mirror.h"

#include "core/math/math_funcs.h"

// Tests for the CPU mirror of `gs_compute_receiver_bias` in
// gs_lighting_common.glsl. The mirror lives in lighting_bias_cpu_mirror.h
// and must stay in lockstep with the shader. Shader changes without a
// mirror update (or vice-versa) will make these tests misleading —
// review both files together.
//
// Issue #295 background: three GLSL paths previously computed
// receiver_bias differently (see commit message). All three now route
// through the same helper. These tests pin its semantics so the call
// sites stay correct.

#ifdef TESTS_ENABLED

TEST_CASE("[GaussianSplatting][LightingBias] per-splat path scales by radius and clamps to min") {
	using namespace GaussianSplatting::LightingBiasCPUMirror;

	// Config matches a typical scene: scale=0.1 (10% of radius added),
	// min=0.005 floor, max disabled (0.0 sentinel).
	ShadowBiasConfig cfg{ 0.1f, 0.005f, 0.0f };

	// Tiny radius -> scale*radius below the min floor; min wins.
	CHECK(Math::is_equal_approx(gs_compute_receiver_bias(cfg, 0.01f), 0.005f));

	// Medium radius (0.1) -> scale*radius = 0.01 > min; bias scales.
	CHECK(Math::is_equal_approx(gs_compute_receiver_bias(cfg, 0.1f), 0.01f));

	// Large radius (1.0) -> scale*radius = 0.1; still uses scaled value.
	CHECK(Math::is_equal_approx(gs_compute_receiver_bias(cfg, 1.0f), 0.1f));
}

TEST_CASE("[GaussianSplatting][LightingBias] resolve path (radius=0) collapses to min") {
	using namespace GaussianSplatting::LightingBiasCPUMirror;

	// The resolve directional/full-lighting paths pass radius=0 because
	// no per-pixel representative radius is plumbed through yet. Result
	// must be exactly the documented minimum (the regression: the bug
	// fixed by #295 returned cfg.scale instead — i.e. 0.1 here, far
	// from the intended 0.005 floor).
	ShadowBiasConfig cfg{ 0.1f, 0.005f, 0.0f };
	CHECK(Math::is_equal_approx(gs_compute_receiver_bias(cfg, 0.0f), 0.005f));

	// Different config: ensure the result tracks .y, not .x.
	ShadowBiasConfig cfg2{ 0.25f, 0.002f, 0.0f };
	CHECK(Math::is_equal_approx(gs_compute_receiver_bias(cfg2, 0.0f), 0.002f));
}

TEST_CASE("[GaussianSplatting][LightingBias] max clamp engages only when max > 0") {
	using namespace GaussianSplatting::LightingBiasCPUMirror;

	// Max=0.05 enabled (>0). Large radius would push bias to 0.5, but
	// the clamp pins it to 0.05.
	ShadowBiasConfig cfg{ 0.5f, 0.001f, 0.05f };
	CHECK(Math::is_equal_approx(gs_compute_receiver_bias(cfg, 1.0f), 0.05f));

	// Same cfg, tiny radius -> min still wins under the clamp.
	CHECK(Math::is_equal_approx(gs_compute_receiver_bias(cfg, 0.0f), 0.001f));

	// Max=0.0 sentinel disables the clamp; bias scales unbounded.
	ShadowBiasConfig cfg_no_max{ 0.5f, 0.001f, 0.0f };
	CHECK(Math::is_equal_approx(gs_compute_receiver_bias(cfg_no_max, 1.0f), 0.5f));
}

TEST_CASE("[GaussianSplatting][LightingBias] fix for #295 — resolve no longer leaks the scale value as bias") {
	using namespace GaussianSplatting::LightingBiasCPUMirror;

	// Pre-fix Path 2 (tile_resolve.glsl:210) used
	//     receiver_bias = params.shadow_bias_config.x;
	// directly — i.e. the radius multiplier (typically 0.05–0.5) was
	// fed into shadow sampling as if it were a world-space bias. With
	// realistic scale values that produces grossly over-biased shadows
	// (no contact, "floating" splats). The corrected Path 2 calls the
	// shared helper with radius=0 and gets the documented .y minimum.
	ShadowBiasConfig cfg{ 0.25f, 0.005f, 0.05f };
	const float fixed = gs_compute_receiver_bias(cfg, 0.0f);
	const float pre_fix_buggy_value = cfg.scale; // would have been 0.25
	CHECK(Math::is_equal_approx(fixed, 0.005f));
	CHECK_FALSE(Math::is_equal_approx(fixed, pre_fix_buggy_value));
}

#endif // TESTS_ENABLED

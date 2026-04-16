/**
 * @file gaussian_data_color_grading.cpp
 * @brief Companion .cpp for gaussian_data.h containing color grading
 *        bake/restore methods.
 *
 * These methods apply, revert, and evaluate color grading transformations
 * on the SH DC coefficients stored in each Gaussian.  They are split out
 * from the main gaussian_data.cpp to keep that file focused on core
 * storage, spatial queries, and GPU upload paths.
 */

#include "gaussian_data.h"

#include "../resources/color_grading_resource.h"
#include "core/error/error_macros.h"

namespace {

struct ColorGradingCpuParams {
    bool enabled = false;
    float exposure_mult = 1.0f;
    float temp_half = 0.0f;
    float tint_half = 0.0f;
    float tint_quarter = 0.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float hue_shift_normalized = 0.0f;
};

static ColorGradingCpuParams _build_color_grading_cpu_params(const Ref<ColorGradingResource> &p_grading) {
    ColorGradingCpuParams params;
    params.enabled = p_grading->get_enabled();
    if (!params.enabled) {
        return params;
    }

    params.exposure_mult = Math::pow(2.0f, p_grading->get_exposure());
    const float temp_factor = p_grading->get_temperature() * 0.01f;
    const float tint_factor = p_grading->get_tint() * 0.01f;
    params.temp_half = temp_factor * 0.5f;
    params.tint_half = tint_factor * 0.5f;
    params.tint_quarter = tint_factor * 0.25f;
    params.contrast = p_grading->get_contrast();
    params.saturation = p_grading->get_saturation();
    params.hue_shift_normalized = p_grading->get_hue_shift() / 360.0f;
    return params;
}

static Color _apply_color_grading_cpu_with_params(const Color &p_color, const ColorGradingCpuParams &p_params) {
    Color result = p_color;

    if (!p_params.enabled) {
        return result;
    }

    // 1. Exposure
    result.r *= p_params.exposure_mult;
    result.g *= p_params.exposure_mult;
    result.b *= p_params.exposure_mult;

    // 2. Temperature & Tint
    result.r += p_params.temp_half;
    result.b -= p_params.temp_half;
    result.g += p_params.tint_half;
    result.r -= p_params.tint_quarter;
    result.b -= p_params.tint_quarter;

    result.r = MAX(result.r, 0.0f);
    result.g = MAX(result.g, 0.0f);
    result.b = MAX(result.b, 0.0f);

    // 3. Contrast
    result.r = (result.r - 0.5f) * p_params.contrast + 0.5f;
    result.g = (result.g - 0.5f) * p_params.contrast + 0.5f;
    result.b = (result.b - 0.5f) * p_params.contrast + 0.5f;

    // 4. Saturation & Hue shift (RGB -> HSV -> adjust -> RGB)
    float h = result.get_h();
    float s = result.get_s();
    float v = result.get_v();

    // Adjust saturation
    s *= p_params.saturation;
    s = CLAMP(s, 0.0f, 1.0f);

    // Adjust hue
    h += p_params.hue_shift_normalized;
    h = Math::fposmod(h, 1.0f); // Wrap around

    result = Color::from_hsv(h, s, v);

    // Final clamp
    result.r = CLAMP(result.r, 0.0f, 65504.0f);
    result.g = CLAMP(result.g, 0.0f, 65504.0f);
    result.b = CLAMP(result.b, 0.0f, 65504.0f);

    return result;
}

} // namespace

Error GaussianData::bake_color_grading(const Ref<ColorGradingResource> &p_grading) {
    ERR_FAIL_COND_V(!p_grading.is_valid(), ERR_INVALID_PARAMETER);
    RWLockWrite lock(data_rwlock);
    const uint32_t gaussian_count = gaussians.size();
    ERR_FAIL_COND_V(gaussian_count == 0, ERR_INVALID_DATA);

    // Backup original SH DC coefficients (only once)
    if (!bake_info.is_baked) {
        bake_info.original_sh_dc.resize(gaussian_count);
        for (uint32_t i = 0; i < gaussian_count; i++) {
            bake_info.original_sh_dc[i] = gaussians[i].sh_dc;
        }
    }

    const ColorGradingCpuParams grading_params = _build_color_grading_cpu_params(p_grading);

    // Apply color grading to each Gaussian's DC coefficients
    for (uint32_t i = 0; i < gaussian_count; i++) {
        Gaussian &g = gaussians[i];

        // Extract base color from SH DC coefficients
        Color base_color = g.sh_dc;

        // Apply color grading
        Color graded_color = _apply_color_grading_cpu_with_params(base_color, grading_params);

        // Write back to SH DC coefficients (preserve alpha)
        g.sh_dc.r = graded_color.r;
        g.sh_dc.g = graded_color.g;
        g.sh_dc.b = graded_color.b;
        // sh_dc.a remains unchanged
    }

    bake_info.is_baked = true;
    bake_info.applied_grading = p_grading;

    // Mark dirty to trigger GPU re-upload
    _on_gaussian_storage_changed_locked();

    return OK;
}

void GaussianData::restore_original_colors() {
    RWLockWrite lock(data_rwlock);
    if (!bake_info.is_baked) {
        return;  // Nothing to restore
    }

    const uint32_t gaussian_count = gaussians.size();
    ERR_FAIL_COND(bake_info.original_sh_dc.size() != gaussian_count);

    // Restore original SH DC coefficients
    for (uint32_t i = 0; i < gaussian_count; i++) {
        gaussians[i].sh_dc = bake_info.original_sh_dc[i];
    }

    bake_info.is_baked = false;
    bake_info.applied_grading.unref();

    // Mark dirty to trigger GPU re-upload
    _on_gaussian_storage_changed_locked();
}

Color GaussianData::apply_color_grading_cpu(const Color &p_color, const Ref<ColorGradingResource> &p_grading) {
    const ColorGradingCpuParams grading_params = _build_color_grading_cpu_params(p_grading);
    return _apply_color_grading_cpu_with_params(p_color, grading_params);
}

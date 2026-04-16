#pragma once

#include "test_macros.h"

#include "../core/streaming_quantization.h"

#include <cmath>

namespace TestGaussianSplatting {

namespace {

static Gaussian make_quant_gaussian(const Vector3 &p_position, const Vector3 &p_scale) {
    Gaussian gaussian = {};
    gaussian.position = p_position;
    gaussian.scale = p_scale;
    return gaussian;
}

static void check_error_bounded(const Vector3 &p_expected, const Vector3 &p_actual, float p_bound) {
    CHECK(std::fabs(p_expected.x - p_actual.x) <= p_bound);
    CHECK(std::fabs(p_expected.y - p_actual.y) <= p_bound);
    CHECK(std::fabs(p_expected.z - p_actual.z) <= p_bound);
}

} // namespace

TEST_CASE("[GaussianSplatting][Quantization] Invalid chunk range clears quantization state") {
    LocalVector<Gaussian> gaussians;
    gaussians.push_back(make_quant_gaussian(Vector3(1.0f, 2.0f, 3.0f), Vector3(0.5f, 0.5f, 0.5f)));
    gaussians.push_back(make_quant_gaussian(Vector3(4.0f, 5.0f, 6.0f), Vector3(1.0f, 1.0f, 1.0f)));

    ChunkQuantizationInfo info;
    info.compute_from_gaussians(gaussians, 1u, 2u, 10u, 8u, true);

    CHECK(info.position_min == Vector3());
    CHECK(info.position_max == Vector3());
    CHECK(info.scale_min == Vector3());
    CHECK(info.scale_max == Vector3());
    CHECK(info.position_range == Vector3());
    CHECK(info.scale_range == Vector3());
    CHECK(info.position_bits == 16u);
    CHECK(info.scale_bits == 12u);
    CHECK_FALSE(info.scales_quantized);
}

TEST_CASE("[GaussianSplatting][Quantization] Degenerate chunk keeps finite ranges and bounded error") {
    LocalVector<Gaussian> gaussians;
    gaussians.push_back(make_quant_gaussian(Vector3(3.0f, -2.0f, 9.0f), Vector3(0.25f, 0.25f, 0.25f)));
    gaussians.push_back(make_quant_gaussian(Vector3(3.0f, -2.0f, 9.0f), Vector3(0.25f, 0.25f, 0.25f)));
    gaussians.push_back(make_quant_gaussian(Vector3(3.0f, -2.0f, 9.0f), Vector3(0.25f, 0.25f, 0.25f)));

    ChunkQuantizationInfo info;
    info.compute_from_gaussians(gaussians, 0u, 3u, 10u, 8u, true);

    CHECK(info.position_range.x >= 1e-6f);
    CHECK(info.position_range.y >= 1e-6f);
    CHECK(info.position_range.z >= 1e-6f);
    CHECK(info.scale_range.x >= 1e-6f);
    CHECK(info.scale_range.y >= 1e-6f);
    CHECK(info.scale_range.z >= 1e-6f);

    uint32_t qx = 0;
    uint32_t qy = 0;
    uint32_t qz = 0;
    info.quantize_position(Vector3(3.0f, -2.0f, 9.0f), qx, qy, qz);
    CHECK(qx == 0u);
    CHECK(qy == 0u);
    CHECK(qz == 0u);

    const Vector3 position_roundtrip = info.dequantize_position(qx, qy, qz);
    CHECK(position_roundtrip == info.position_min);

    info.quantize_scale(Vector3(0.25f, 0.25f, 0.25f), qx, qy, qz);
    CHECK(qx == 0u);
    CHECK(qy == 0u);
    CHECK(qz == 0u);

    const Vector3 scale_roundtrip = info.dequantize_scale(qx, qy, qz);
    CHECK(scale_roundtrip == info.scale_min);
    CHECK(info.get_max_position_error() > 0.0f);
    CHECK(info.get_max_scale_error() > 0.0f);
}

TEST_CASE("[GaussianSplatting][Quantization] Position and scale quantization clamp and respect max error bounds") {
    LocalVector<Gaussian> gaussians;
    gaussians.push_back(make_quant_gaussian(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.5f, 1.0f, 2.0f)));
    gaussians.push_back(make_quant_gaussian(Vector3(10.0f, 20.0f, 30.0f), Vector3(1.5f, 3.0f, 6.0f)));

    ChunkQuantizationInfo info;
    info.compute_from_gaussians(gaussians, 0u, 2u, 8u, 10u, true);

    uint32_t qx = 0;
    uint32_t qy = 0;
    uint32_t qz = 0;
    info.quantize_position(Vector3(-5.0f, 10.0f, 35.0f), qx, qy, qz);
    CHECK(qx == 0u);
    CHECK(qy == 128u);
    CHECK(qz == 255u);

    const Vector3 clamped_position = info.dequantize_position(qx, qy, qz);
    CHECK(clamped_position.x >= info.position_min.x);
    CHECK(clamped_position.x <= info.position_max.x);
    CHECK(clamped_position.z >= info.position_min.z);
    CHECK(clamped_position.z <= info.position_max.z);
    CHECK(clamped_position.y == doctest::Approx(10.0f).epsilon(0.01f));

    const float expected_position_error = 30.0f / (255.0f * 2.0f);
    CHECK(info.get_max_position_error() == doctest::Approx(expected_position_error).epsilon(0.0001f));

    const Vector3 original_scale(1.25f, 2.5f, 5.0f);
    info.quantize_scale(original_scale, qx, qy, qz);
    const Vector3 roundtrip_scale = info.dequantize_scale(qx, qy, qz);
    check_error_bounded(original_scale, roundtrip_scale, info.get_max_scale_error() + 1e-6f);

    info.quantize_scale(Vector3(-1.0f, 2.0f, 10.0f), qx, qy, qz);
    CHECK(qx == 0u);
    CHECK(qy == 512u);
    CHECK(qz == 1023u);
}

TEST_CASE("[GaussianSplatting][Quantization] Scale quantization disabled returns zeroed scale contract") {
    LocalVector<Gaussian> gaussians;
    gaussians.push_back(make_quant_gaussian(Vector3(2.0f, 4.0f, 8.0f), Vector3(0.3f, 0.6f, 0.9f)));
    gaussians.push_back(make_quant_gaussian(Vector3(3.0f, 5.0f, 13.0f), Vector3(0.8f, 1.2f, 1.6f)));

    ChunkQuantizationInfo info;
    info.compute_from_gaussians(gaussians, 0u, 2u, 12u, 9u, false);

    uint32_t qx = 123u;
    uint32_t qy = 456u;
    uint32_t qz = 789u;
    info.quantize_scale(Vector3(100.0f, 200.0f, 300.0f), qx, qy, qz);
    CHECK(qx == 0u);
    CHECK(qy == 0u);
    CHECK(qz == 0u);
    CHECK(info.dequantize_scale(1u, 2u, 3u) == Vector3());
    CHECK(info.get_max_scale_error() == 0.0f);
}

} // namespace TestGaussianSplatting

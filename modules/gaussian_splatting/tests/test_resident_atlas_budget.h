#pragma once

#include "test_macros.h"

#include "../renderer/resident_atlas_budget.h"

namespace ResidentAtlasBudgetTests {

static constexpr uint64_t k2GB = uint64_t(2) * 1024 * 1024 * 1024;
static constexpr uint64_t k1GB = uint64_t(1) * 1024 * 1024 * 1024;
static constexpr uint64_t k8GB = uint64_t(8) * 1024 * 1024 * 1024;

static Gaussian make_g(float p_opacity, float p_scale, float p_marker) {
    Gaussian g = {};
    g.opacity = p_opacity;
    g.scale = Vector3(p_scale, p_scale * 0.5f, p_scale * 0.25f); // max axis == p_scale
    g.position = Vector3(p_marker, 0.0f, 0.0f); // marker to identify after compaction
    return g;
}

} // namespace ResidentAtlasBudgetTests

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Unknown / below-floor device budget keeps the 2 GB staging cap") {
    using namespace ResidentAtlasBudget;
    // device budget unknown (0) -> capacity unknown -> do-no-harm, cap stays staging ceiling.
    CHECK(compute_effective_atlas_cap_bytes(0, 0) == ResidentAtlasBudgetTests::k2GB);
    // 200 MiB is below the 256 MiB sane floor -> treated as unknown.
    CHECK(compute_effective_atlas_cap_bytes(uint64_t(200) * 1024 * 1024, 0) == ResidentAtlasBudgetTests::k2GB);
    // exactly at the floor is trusted.
    CHECK(compute_effective_atlas_cap_bytes(kMinTrustedDeviceBudgetBytes, 0) ==
            uint64_t(double(kMinTrustedDeviceBudgetBytes) * kResidentAtlasHeadroomFraction));
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Capable GPU is byte-identical to today (staging dominates)") {
    using namespace ResidentAtlasBudget;
    // 8 GB device: 0.60*8GB = 4.8 GB, MIN with 2 GB staging => 2 GB (no clamp on capable GPUs).
    CHECK(compute_effective_atlas_cap_bytes(ResidentAtlasBudgetTests::k8GB, 0) == ResidentAtlasBudgetTests::k2GB);
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Low-end device budget binds beneath the staging cap") {
    using namespace ResidentAtlasBudget;
    // 1 GB device: 0.60*1GB binds (< 2 GB staging).
    const uint64_t expected = uint64_t(double(ResidentAtlasBudgetTests::k1GB) * kResidentAtlasHeadroomFraction);
    CHECK(compute_effective_atlas_cap_bytes(ResidentAtlasBudgetTests::k1GB, 0) == expected);
    CHECK(expected < ResidentAtlasBudgetTests::k2GB);
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Override forces a clamp and never loosens (MIN semantics)") {
    using namespace ResidentAtlasBudget;
    // override forces a clamp even on a huge GPU (the validation lever).
    CHECK(compute_effective_atlas_cap_bytes(ResidentAtlasBudgetTests::k8GB, 512) == uint64_t(512) * 1024 * 1024);
    // an override larger than the budget term cannot loosen the cap.
    const uint64_t budget_cap = uint64_t(double(ResidentAtlasBudgetTests::k1GB) * kResidentAtlasHeadroomFraction);
    CHECK(compute_effective_atlas_cap_bytes(ResidentAtlasBudgetTests::k1GB, 4096) == budget_cap);
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Subset plan: full atlas fits -> not reduced") {
    using namespace ResidentAtlasBudget;
    const SubsetPlan plan = compute_subset_plan(100000, ResidentAtlasBudgetTests::k2GB, 144);
    CHECK_FALSE(plan.reduced);
    CHECK(plan.keep_ratio == doctest::Approx(1.0));
    CHECK(plan.target_keep == 100000);
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Subset plan: over budget -> reduced, target never exceeds cap") {
    using namespace ResidentAtlasBudget;
    const uint64_t cap = compute_effective_atlas_cap_bytes(ResidentAtlasBudgetTests::k1GB, 0);
    const SubsetPlan plan = compute_subset_plan(20000000, cap, 144);
    CHECK(plan.reduced);
    CHECK(plan.target_keep < plan.source_count);
    CHECK(plan.keep_ratio < 1.0);
    // The soft splat target must never imply more bytes than the cap.
    CHECK(plan.target_keep * uint64_t(144) <= cap);
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Per-chunk keep count floors at 1 and respects the ratio") {
    using namespace ResidentAtlasBudget;
    CHECK(compute_chunk_keep_count(0, 0.5) == 0);          // empty stays empty
    CHECK(compute_chunk_keep_count(10, 1.0) == 10);        // ratio >= 1 keeps all
    CHECK(compute_chunk_keep_count(10, 1.5) == 10);        // clamped to count
    CHECK(compute_chunk_keep_count(10, 0.0001) == 1);      // floor: never delete a region
    CHECK(compute_chunk_keep_count(10, 0.0) == 1);
    CHECK(compute_chunk_keep_count(10, 0.4) == 4);         // round(4.0)
    CHECK(compute_chunk_keep_count(10, 0.45) == 5);        // round(4.5)
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Top-K selection: highest importance, ascending, deterministic ties") {
    using namespace ResidentAtlasBudget;
    const float importance[5] = { 0.01f, 0.90f, 0.25f, 0.04f, 2.00f };
    LocalVector<uint32_t> keep;
    select_top_k_indices(importance, 5, 2, keep);
    REQUIRE(keep.size() == 2);
    CHECK(keep[0] == 1u); // returned ascending
    CHECK(keep[1] == 4u); // indices of the two largest (0.90, 2.00)

    // determinism: identical input -> identical output.
    LocalVector<uint32_t> keep2;
    select_top_k_indices(importance, 5, 2, keep2);
    REQUIRE(keep2.size() == 2);
    CHECK(keep2[0] == keep[0]);
    CHECK(keep2[1] == keep[1]);

    // ties broken by ascending index.
    const float tied[3] = { 1.0f, 1.0f, 1.0f };
    LocalVector<uint32_t> keep_tied;
    select_top_k_indices(tied, 3, 2, keep_tied);
    REQUIRE(keep_tied.size() == 2);
    CHECK(keep_tied[0] == 0u);
    CHECK(keep_tied[1] == 1u);

    // keep >= count returns the identity range.
    LocalVector<uint32_t> keep_all;
    select_top_k_indices(importance, 5, 9, keep_all);
    REQUIRE(keep_all.size() == 5);
    CHECK(keep_all[0] == 0u);
    CHECK(keep_all[4] == 4u);
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Chunk compaction keeps top importance and SH stays parallel") {
    using namespace ResidentAtlasBudget;
    using namespace ResidentAtlasBudgetTests;

    LocalVector<Gaussian> gaussians;
    gaussians.push_back(make_g(0.1f, 0.1f, 0.0f)); // idx0 importance ~0.010
    gaussians.push_back(make_g(0.9f, 1.0f, 1.0f)); // idx1 importance ~0.900
    gaussians.push_back(make_g(0.5f, 0.5f, 2.0f)); // idx2 importance ~0.250
    gaussians.push_back(make_g(0.2f, 0.2f, 3.0f)); // idx3 importance ~0.040
    gaussians.push_back(make_g(1.0f, 2.0f, 4.0f)); // idx4 importance ~2.000

    const uint32_t stride = 15;
    LocalVector<Vector3> sh;
    sh.resize(5 * stride);
    for (uint32_t i = 0; i < 5; i++) {
        for (uint32_t c = 0; c < stride; c++) {
            sh[i * stride + c] = Vector3(float(i), float(c), 0.0f); // x encodes source index
        }
    }

    // keep_ratio 0.4 over count 5 -> round(2.0) == 2 kept (the two highest: idx1, idx4).
    const uint32_t kept = compact_chunk_by_importance(gaussians, sh, stride, 0.4);
    REQUIRE(kept == 2);
    REQUIRE(gaussians.size() == 2);
    REQUIRE(sh.size() == 2u * stride);

    // Ascending-index compaction => slot0 is source idx1, slot1 is source idx4.
    CHECK(gaussians[0].position.x == doctest::Approx(1.0f));
    CHECK(gaussians[1].position.x == doctest::Approx(4.0f));
    // SH block of each kept gaussian must match its original source block.
    CHECK(sh[0 * stride + 0].x == doctest::Approx(1.0f));
    CHECK(sh[0 * stride + 7].x == doctest::Approx(1.0f));
    CHECK(sh[1 * stride + 0].x == doctest::Approx(4.0f));
    CHECK(sh[1 * stride + 7].x == doctest::Approx(4.0f));
    CHECK(sh[1 * stride + 7].y == doctest::Approx(7.0f)); // coefficient slot preserved
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Compaction is a no-op when the ratio keeps everything") {
    using namespace ResidentAtlasBudget;
    using namespace ResidentAtlasBudgetTests;

    LocalVector<Gaussian> gaussians;
    for (uint32_t i = 0; i < 4; i++) {
        gaussians.push_back(make_g(0.5f, 1.0f, float(i)));
    }
    LocalVector<Vector3> sh; // no high-order SH (stride 0) is a valid path
    const uint32_t kept = compact_chunk_by_importance(gaussians, sh, 0, 1.0);
    CHECK(kept == 4);
    REQUIRE(gaussians.size() == 4);
    CHECK(gaussians[0].position.x == doctest::Approx(0.0f));
    CHECK(gaussians[3].position.x == doctest::Approx(3.0f));
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Single global ratio thins multiple chunks proportionally within cap") {
    using namespace ResidentAtlasBudget;
    // Two assets/chunks of different sizes share ONE global keep_ratio.
    const uint64_t cap = compute_effective_atlas_cap_bytes(ResidentAtlasBudgetTests::k1GB, 0);
    const SubsetPlan plan = compute_subset_plan(20000000, cap, 144);
    REQUIRE(plan.reduced);

    const uint32_t chunk_a = 1000;
    const uint32_t chunk_b = 250;
    const uint32_t keep_a = compute_chunk_keep_count(chunk_a, plan.keep_ratio);
    const uint32_t keep_b = compute_chunk_keep_count(chunk_b, plan.keep_ratio);
    // Same ratio applied to both -> proportional thinning, each >= 1, each < its source.
    CHECK(keep_a >= 1u);
    CHECK(keep_b >= 1u);
    CHECK(keep_a < chunk_a);
    CHECK(keep_b < chunk_b);
    CHECK(double(keep_a) / double(chunk_a) == doctest::Approx(double(keep_b) / double(chunk_b)).epsilon(0.05));
}

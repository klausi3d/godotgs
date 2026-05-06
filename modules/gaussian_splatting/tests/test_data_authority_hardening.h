/**************************************************************************/
/*  test_data_authority_hardening.h                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

// Tests pinning the Wave 2 data authority hardening rules:
//   - GaussianData is the only mutable runtime truth.
//   - Its bulk mutators invalidate derived state (octree, animation cache).
//   - apply_animation_at_time() is gone; animation stays a non-destructive
//     view through the get_animated_*() accessors.
//   - GaussianSplatAsset packed setters are not a runtime mutation path.
//     Once the asset has handed out a GaussianData the payload is sealed.
//   - Raw storage accessors document their thread contract and fire a
//     dev-build diagnostic for obvious misuse.

#include "../animation/animation_state_machine.h"
#include "../core/gaussian_data.h"
#include "../core/gaussian_splat_asset.h"
#include "core/io/resource.h"
#include "core/os/thread.h"
#include "core/templates/local_vector.h"
#include "tests/test_macros.h"

#include <atomic>
#include <type_traits>
#include <utility>

namespace TestGaussianSplatting {

// Namespace-scope SFINAE probe used to pin apply_animation_at_time() as
// permanently removed. MSVC disallows template members inside a local
// struct, so the detector lives at namespace scope.
template <typename T, typename = void>
struct _HasApplyAnimationAtTime : std::false_type {};

template <typename T>
struct _HasApplyAnimationAtTime<T,
        decltype((void)std::declval<T *>()->apply_animation_at_time(0.0f))>
        : std::true_type {};

namespace {

Gaussian _make_authority_test_splat(const Vector3 &p_position,
        const Color &p_color = Color(1, 1, 1, 1),
        const Vector3 &p_scale = Vector3(1, 1, 1)) {
    Gaussian g;
    g.position = p_position;
    g.scale = p_scale;
    g.rotation = Quaternion();
    g.opacity = p_color.a;
    g.sh_dc = p_color;
    g.normal = Vector3(0, 1, 0);
    g.area = 1.0f;
    g.brush_axes = Vector2(1.0f, 1.0f);
    g.stroke_age = 0.0f;
    g.painterly_meta = 0;
    g.render_meta = 0;
    return g;
}

LocalVector<Gaussian> _make_authority_test_cloud() {
    LocalVector<Gaussian> splats;
    splats.resize(4);
    splats[0] = _make_authority_test_splat(Vector3(-2, 0, 0), Color(1, 0, 0, 1));
    splats[1] = _make_authority_test_splat(Vector3(2, 0, 0), Color(0, 1, 0, 1));
    splats[2] = _make_authority_test_splat(Vector3(0, -2, 0), Color(0, 0, 1, 1));
    splats[3] = _make_authority_test_splat(Vector3(0, 2, 0), Color(1, 1, 0, 1));
    return splats;
}

#ifdef THREADS_ENABLED
struct _AnimatedAccessorRaceContext {
    Ref<::GaussianData> data;
    uint32_t splat_count = 0;
    std::atomic<bool> stop{ false };
    std::atomic<int> invalid_samples{ 0 };
};

static bool _is_finite_color(const Color &p_color) {
    return Math::is_finite(p_color.r) &&
            Math::is_finite(p_color.g) &&
            Math::is_finite(p_color.b) &&
            Math::is_finite(p_color.a);
}

static void _animated_accessor_reader_thread(void *p_userdata) {
    _AnimatedAccessorRaceContext *ctx = static_cast<_AnimatedAccessorRaceContext *>(p_userdata);
    if (!ctx || !ctx->data.is_valid()) {
        return;
    }

    while (!ctx->stop.load(std::memory_order_acquire)) {
        for (uint32_t i = 0; i < ctx->splat_count; i++) {
            const int index = static_cast<int>(i);
            const Vector3 position = ctx->data->get_animated_position(index, -1.0f);
            const Color color = ctx->data->get_animated_color(index, -1.0f);
            const float opacity = ctx->data->get_animated_opacity(index, -1.0f);
            const Vector3 scale = ctx->data->get_animated_scale(index, -1.0f);
            const Quaternion rotation = ctx->data->get_animated_rotation(index, -1.0f);
            if (!position.is_finite() || !_is_finite_color(color) ||
                    !Math::is_finite(opacity) || !scale.is_finite() ||
                    !rotation.is_finite()) {
                ctx->invalid_samples.fetch_add(1, std::memory_order_relaxed);
            }
        }

        if (ctx->data->get_animated_position(-1, 0.0f) != Vector3() ||
                ctx->data->get_animated_color(-1, 0.0f) != Color() ||
                ctx->data->get_animated_opacity(-1, 0.0f) != 1.0f ||
                ctx->data->get_animated_scale(-1, 0.0f) != Vector3(1, 1, 1) ||
                ctx->data->get_animated_rotation(-1, 0.0f) != Quaternion()) {
            ctx->invalid_samples.fetch_add(1, std::memory_order_relaxed);
        }

        Thread::yield();
    }
}
#endif

} // namespace

TEST_CASE("[GaussianSplatting][DataAuthority] Bulk setters invalidate octree and animation caches") {
    Ref<::GaussianData> data;
    data.instantiate();
    data->set_gaussians(_make_authority_test_cloud());

    // Build an octree anchored to the initial positions and query a cell that
    // matches splat[0] at (-2, 0, 0). Query should return at least one hit.
    data->build_octree();
    TypedArray<int> hits_before = data->query_octree(AABB(Vector3(-3, -1, -1), Vector3(2, 2, 2)));
    CHECK_MESSAGE(hits_before.size() > 0, "Octree should index splat 0 at (-2,0,0) prior to bulk mutation.");

    // Move every splat far away. Under the pre-fix behavior this did not
    // invalidate the octree, so query_octree() would still report hits near
    // the old positions. The hardening flushes the octree on bulk set_positions.
    PackedVector3Array far_positions;
    far_positions.resize(4);
    far_positions.set(0, Vector3(500, 500, 500));
    far_positions.set(1, Vector3(501, 500, 500));
    far_positions.set(2, Vector3(500, 501, 500));
    far_positions.set(3, Vector3(500, 500, 501));
    data->set_positions(far_positions);

    TypedArray<int> hits_after_move = data->query_octree(AABB(Vector3(-3, -1, -1), Vector3(2, 2, 2)));
    CHECK_MESSAGE(hits_after_move.size() == 0,
            "set_positions() must invalidate the octree so old positions do not leak.");

    // Rebuild octree at the new location; the new query should now find splats.
    data->build_octree();
    TypedArray<int> hits_new = data->query_octree(AABB(Vector3(499, 499, 499), Vector3(3, 3, 3)));
    CHECK(hits_new.size() > 0);

    // set_scales/set_rotations/set_opacities all feed derived bounds+caches
    // too; verify they at least bump the content revision so dependent
    // consumers re-read the payload.
    const uint64_t revision_before_scales = data->get_content_revision();
    PackedVector3Array scales;
    scales.resize(4);
    for (int i = 0; i < 4; i++) {
        scales.set(i, Vector3(2.0f, 2.0f, 2.0f));
    }
    data->set_scales(scales);
    CHECK(data->get_content_revision() > revision_before_scales);

    const uint64_t revision_before_opacities = data->get_content_revision();
    PackedFloat32Array opacities;
    opacities.resize(4);
    for (int i = 0; i < 4; i++) {
        opacities.set(i, 0.25f);
    }
    data->set_opacities(opacities);
    CHECK(data->get_content_revision() > revision_before_opacities);

    const uint64_t revision_before_rotations = data->get_content_revision();
    TypedArray<Quaternion> rotations;
    rotations.resize(4);
    for (int i = 0; i < 4; i++) {
        rotations[i] = Quaternion(Vector3(0, 1, 0), Math::deg_to_rad(45.0f));
    }
    data->set_rotations(rotations);
    CHECK(data->get_content_revision() > revision_before_rotations);
}

TEST_CASE("[GaussianSplatting][DataAuthority] Animation sampling is non-destructive") {
    Ref<::GaussianData> data;
    data.instantiate();
    data->set_gaussians(_make_authority_test_cloud());

    // Snapshot the base positions so we can verify they never shift under us.
    Vector<Vector3> baseline_positions;
    {
        const Gaussian *g = data->get_gaussians();
        REQUIRE(g != nullptr);
        baseline_positions.resize(data->get_count());
        for (int i = 0; i < data->get_count(); i++) {
            baseline_positions.write[i] = g[i].position;
        }
    }

    Ref<GaussianSplatting::GaussianAnimationStateMachine> anim;
    anim.instantiate();
    anim->set_splat_count(data->get_count());
    data->set_animation_state_machine(anim);

    // With no animated tracks, get_animated_position() is a pass-through.
    for (int i = 0; i < data->get_count(); i++) {
        const Vector3 sampled = data->get_animated_position(i, 0.5f);
        CHECK(sampled.is_equal_approx(baseline_positions[i]));
    }

    // Core invariant: calling get_animated_* must NOT mutate the base payload.
    const Gaussian *after_sample = data->get_gaussians();
    REQUIRE(after_sample != nullptr);
    for (int i = 0; i < data->get_count(); i++) {
        CHECK_MESSAGE(after_sample[i].position.is_equal_approx(baseline_positions[i]),
                "Sampling animation must leave the base GaussianData position untouched.");
    }

    // apply_animation_at_time() has been removed from the public API entirely.
    // This compile-time check pins that removal in place: if a future change
    // adds the symbol back, this static_assert stops being well-formed.
    static_assert(!_HasApplyAnimationAtTime<::GaussianData>::value,
            "GaussianData::apply_animation_at_time() must stay removed -- animation is a non-destructive view.");
}

TEST_CASE("[GaussianSplatting][DataAuthority] Animated accessors tolerate concurrent payload mutation") {
#ifndef THREADS_ENABLED
    MESSAGE("Skipping - THREADS_ENABLED is not enabled in this build");
    return;
#else
    constexpr uint32_t splat_count = 64;
    LocalVector<Gaussian> splats;
    splats.resize(splat_count);
    for (uint32_t i = 0; i < splat_count; i++) {
        splats[i] = _make_authority_test_splat(Vector3(float(i), 0.0f, 0.0f),
                Color(float(i % 7) / 6.0f, 0.25f, 0.75f, 0.8f),
                Vector3(1.0f, 1.0f, 1.0f));
    }

    Ref<::GaussianData> data;
    data.instantiate();
    data->set_gaussians(splats);

    Ref<GaussianSplatting::GaussianAnimationStateMachine> animation;
    animation.instantiate();
    animation->set_splat_count(splat_count);
    data->set_animation_state_machine(animation);

    PackedVector3Array positions_a;
    PackedVector3Array positions_b;
    PackedVector3Array scales_a;
    PackedVector3Array scales_b;
    PackedFloat32Array opacities_a;
    PackedFloat32Array opacities_b;
    TypedArray<Quaternion> rotations_a;
    TypedArray<Quaternion> rotations_b;
    positions_a.resize(splat_count);
    positions_b.resize(splat_count);
    scales_a.resize(splat_count);
    scales_b.resize(splat_count);
    opacities_a.resize(splat_count);
    opacities_b.resize(splat_count);
    rotations_a.resize(splat_count);
    rotations_b.resize(splat_count);
    for (uint32_t i = 0; i < splat_count; i++) {
        const int index = static_cast<int>(i);
        positions_a.set(index, Vector3(float(i), 1.0f, 2.0f));
        positions_b.set(index, Vector3(-float(i), 3.0f, 4.0f));
        scales_a.set(index, Vector3(1.0f, 1.5f, 2.0f));
        scales_b.set(index, Vector3(2.0f, 1.0f, 0.5f));
        opacities_a.set(index, 0.25f);
        opacities_b.set(index, 0.85f);
        rotations_a[index] = Quaternion();
        rotations_b[index] = Quaternion(Vector3(0.0f, 1.0f, 0.0f), Math::deg_to_rad(30.0f));
    }

    _AnimatedAccessorRaceContext ctx;
    ctx.data = data;
    ctx.splat_count = splat_count;

    Thread reader_thread;
    const Thread::ID reader_id = reader_thread.start(_animated_accessor_reader_thread, &ctx);
    REQUIRE(reader_id != Thread::UNASSIGNED_ID);

    for (uint32_t iteration = 0; iteration < 96; iteration++) {
        const bool use_a = (iteration % 2) == 0;
        data->set_animation_enabled(use_a);
        data->set_animation_state_machine((iteration % 8) == 0 ? Ref<GaussianSplatting::GaussianAnimationStateMachine>() : animation);
        data->set_positions(use_a ? positions_a : positions_b);
        data->set_scales(use_a ? scales_a : scales_b);
        data->set_opacities(use_a ? opacities_a : opacities_b);
        data->set_rotations(use_a ? rotations_a : rotations_b);
        CHECK(data->get_gaussian(0).position.is_finite());
        Thread::yield();
    }

    ctx.stop.store(true, std::memory_order_release);
    reader_thread.wait_to_finish();

    CHECK(ctx.invalid_samples.load(std::memory_order_relaxed) == 0);
#endif
}

TEST_CASE("[GaussianSplatting][DataAuthority] GaussianSplatAsset packed setters are sealed at runtime") {
    Ref<GaussianSplatAsset> asset;
    asset.instantiate();

    // Fresh assets still populate through the public property surface used by
    // import/deserialization before first runtime promotion.
    PackedFloat32Array bootstrap_positions;
    bootstrap_positions.resize(12);
    for (int i = 0; i < bootstrap_positions.size(); i++) {
        bootstrap_positions.set(i, float(i));
    }
    asset->set_splat_count(4);
    asset->set_positions(bootstrap_positions);
    CHECK(asset->get_positions().size() == bootstrap_positions.size());

    // Populate the asset through the legitimate persistence boundary.
    Ref<::GaussianData> data;
    data.instantiate();
    data->set_gaussians(_make_authority_test_cloud());
    const int splat_count = data->get_count();
    REQUIRE(splat_count > 0);
    REQUIRE(asset->populate_from_gaussian_data(data) == OK);
    CHECK(asset->get_splat_count() == (uint32_t)splat_count);

    // Asset is now sealed: handing out the runtime GaussianData keeps the seal.
    Ref<::GaussianData> runtime = asset->get_gaussian_data();
    REQUIRE(runtime.is_valid());
    CHECK(runtime->get_count() == splat_count);

    // Capture the asset positions buffer before attempting a sealed mutation.
    PackedFloat32Array before_positions = asset->get_positions();

    // Attempting an asset packed setter while sealed must NOT change the
    // underlying payload. We accept either a hard-fail or a silent no-op --
    // the required outcome is that the asset does not treat itself as a live
    // mutable runtime authority.
    PackedFloat32Array mutated;
    mutated.resize(before_positions.size());
    for (int i = 0; i < mutated.size(); i++) {
        mutated.set(i, 999.0f);
    }
    {
        // Suppress the expected ERR_PRINT so the test log stays clean.
        ERR_PRINT_OFF;
        asset->set_positions(mutated);
        ERR_PRINT_ON;
    }
    PackedFloat32Array after_positions = asset->get_positions();
    REQUIRE(after_positions.size() == before_positions.size());
    bool positions_unchanged = true;
    for (int i = 0; i < after_positions.size(); i++) {
        if (!Math::is_equal_approx(after_positions[i], before_positions[i])) {
            positions_unchanged = false;
            break;
        }
    }
    CHECK_MESSAGE(positions_unchanged,
            "Sealed GaussianSplatAsset must reject runtime set_positions() writes.");

    // Public same-count "reseat" must stay rejected once runtime authority
    // has been handed out. Reopening the setters here would recreate split
    // authority between the asset arrays and the live GaussianData.
    {
        ERR_PRINT_OFF;
        asset->set_splat_count(splat_count);
        asset->set_positions(mutated);
        ERR_PRINT_ON;
    }
    PackedFloat32Array after_rejected_reseat = asset->get_positions();
    REQUIRE(after_rejected_reseat.size() == before_positions.size());
    bool reseat_rejected = true;
    for (int i = 0; i < after_rejected_reseat.size(); i++) {
        if (!Math::is_equal_approx(after_rejected_reseat[i], before_positions[i])) {
            reseat_rejected = false;
            break;
        }
    }
    CHECK_MESSAGE(reseat_rejected,
            "Sealed GaussianSplatAsset must reject same-count set_splat_count() reopen attempts.");

    const Gaussian *runtime_ptr = runtime->get_gaussians();
    REQUIRE(runtime_ptr != nullptr);
    CHECK(runtime_ptr[0].position.is_equal_approx(Vector3(-2, 0, 0)));

    // populate_from_gaussian_data() is the legitimate override path and must
    // work regardless of prior seal state.
    Ref<::GaussianData> rewrite_data;
    rewrite_data.instantiate();
    rewrite_data->set_gaussians(_make_authority_test_cloud());
    REQUIRE(asset->populate_from_gaussian_data(rewrite_data) == OK);
}

TEST_CASE("[GaussianSplatting][DataAuthority] Raw storage accessors compile-survivable and main-thread safe") {
    // This test merely exercises the accessor to confirm the debug diagnostic
    // is a no-op on the main thread (fires at most once off-main-thread in
    // dev builds). The important outcome is that legitimate main-thread
    // consumers keep working after the hardening.
    Ref<::GaussianData> data;
    data.instantiate();
    data->set_gaussians(_make_authority_test_cloud());

    const LocalVector<Gaussian> &storage = data->get_gaussian_storage();
    CHECK(storage.size() == 4u);

    const Gaussian *ptr = data->get_gaussians();
    REQUIRE(ptr != nullptr);
    CHECK(ptr[0].position.is_equal_approx(Vector3(-2, 0, 0)));
}

} // namespace TestGaussianSplatting

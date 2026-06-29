#pragma once

#include "test_macros.h"
#include "../animation/animation_state_machine.h"
#include "core/math/math_funcs.h"

TEST_CASE("[GaussianSplatting][Animation] linear keyframe interpolation") {
    using namespace GaussianSplatting;

    GaussianAnimationStateMachine state_machine;
    int clip_index = state_machine.add_clip("linear", 1.0f);
    state_machine.set_splat_count(1);

    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_OPACITY, 0.0f, 0.0f);
    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_OPACITY, 1.0f, 10.0f);
    state_machine.play(clip_index);

    CHECK(Math::is_equal_approx(state_machine.sample_opacity(0, 0.0f), 0.0f));
    CHECK(Math::is_equal_approx(state_machine.sample_opacity(0, 0.5f), 5.0f));
    CHECK(Math::is_equal_approx(state_machine.sample_opacity(0, 1.0f), 10.0f));

    // Test extrapolation (should clamp)
    CHECK(Math::is_equal_approx(state_machine.sample_opacity(0, -0.5f), 0.0f));
    CHECK(Math::is_equal_approx(state_machine.sample_opacity(0, 1.5f), 10.0f));
}

TEST_CASE("[GaussianSplatting][Animation] Vector3 keyframe interpolation") {
    using namespace GaussianSplatting;

    GaussianAnimationStateMachine state_machine;
    int clip_index = state_machine.add_clip("vector", 1.0f);
    state_machine.set_splat_count(1);

    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_POSITION, 0.0f, Vector3(0, 0, 0));
    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_POSITION, 1.0f, Vector3(10, 20, 30));
    state_machine.play(clip_index);

    Vector3 mid = state_machine.sample_position(0, 0.5f);
    CHECK(mid.is_equal_approx(Vector3(5, 10, 15)));
}

TEST_CASE("[GaussianSplatting][Animation] splat-indexed keyframes sample per-splat values") {
    using namespace GaussianSplatting;

    GaussianAnimationStateMachine state_machine;
    int clip_index = state_machine.add_clip("per_splat_position", 1.0f);
    state_machine.set_splat_count(3);

    PackedVector3Array start_positions;
    start_positions.push_back(Vector3(0, 0, 0));
    start_positions.push_back(Vector3(10, 0, 0));
    start_positions.push_back(Vector3(20, 0, 0));

    PackedVector3Array end_positions;
    end_positions.push_back(Vector3(10, 0, 0));
    end_positions.push_back(Vector3(20, 0, 0));
    end_positions.push_back(Vector3(30, 0, 0));

    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_POSITION, 0.0f, start_positions);
    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_POSITION, 1.0f, end_positions);
    state_machine.play(clip_index);

    CHECK(state_machine.sample_position(0, 0.5f).is_equal_approx(Vector3(5, 0, 0)));
    CHECK(state_machine.sample_position(1, 0.5f).is_equal_approx(Vector3(15, 0, 0)));
    CHECK(state_machine.sample_position(2, 0.5f).is_equal_approx(Vector3(25, 0, 0)));
}

TEST_CASE("[GaussianSplatting][Animation] mixed scalar and per-splat keyframes only reject missing indices in sampled window") {
    using namespace GaussianSplatting;

    GaussianAnimationStateMachine state_machine;
    int clip_index = state_machine.add_clip("mixed_window", 2.0f);
    state_machine.set_splat_count(2);

    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_POSITION, 0.0f, Vector3(0, 0, 0));
    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_POSITION, 1.0f, Vector3(10, 0, 0));

    PackedVector3Array per_splat_tail;
    per_splat_tail.push_back(Vector3(30, 0, 0)); // Only splat 0 provided.
    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_POSITION, 2.0f, per_splat_tail);
    state_machine.play(clip_index);

    // Sample window [0, 1] uses scalar keyframes; missing splat-1 entry at t=2 should not invalidate this.
    CHECK(state_machine.sample_position(1, 0.5f).is_equal_approx(Vector3(5, 0, 0)));
    CHECK(state_machine.sample_position(1, 1.0f).is_equal_approx(Vector3(10, 0, 0)));

    // Sample window [1, 2] includes a per-splat keyframe without splat-1 data and should fail gracefully.
    CHECK(state_machine.sample_position(1, 1.5f).is_equal_approx(Vector3()));
}

TEST_CASE("[GaussianSplatting][Animation] global track sampling stays index-agnostic in batch APIs") {
    using namespace GaussianSplatting;

    GaussianAnimationStateMachine state_machine;
    int clip_index = state_machine.add_clip("batch", 1.0f);
    state_machine.set_splat_count(4);

    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_OPACITY, 0.0f, 0.0f);
    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_OPACITY, 1.0f, 1.0f);
    state_machine.play(clip_index);

    const float sample_time = 0.25f;
    const float expected = state_machine.sample_opacity(0, sample_time);
    CHECK(Math::is_equal_approx(state_machine.sample_opacity(3, sample_time), expected));

    LocalVector<float> opacities;
    state_machine.sample_opacities_batch(opacities, sample_time);

    REQUIRE(opacities.size() == 4);
    for (uint32_t i = 0; i < opacities.size(); i++) {
        CHECK(Math::is_equal_approx(opacities[i], expected));
    }
}

TEST_CASE("[GaussianSplatting][Animation] integer opacity keyframes sample consistently across scalar and batch APIs") {
    using namespace GaussianSplatting;

    GaussianAnimationStateMachine state_machine;
    int clip_index = state_machine.add_clip("int_opacity", 1.0f);
    state_machine.set_splat_count(3);

    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_OPACITY, 0.0f, 0);
    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_OPACITY, 1.0f, 2);
    state_machine.play(clip_index);

    const float sample_time = 0.5f;
    const float expected = state_machine.sample_opacity(0, sample_time);
    CHECK(Math::is_equal_approx(expected, 1.0f));
    CHECK(Math::is_equal_approx(state_machine.sample_opacity(2, sample_time), expected));

    LocalVector<float> opacities;
    state_machine.sample_opacities_batch(opacities, sample_time);
    REQUIRE(opacities.size() == 3);
    for (uint32_t i = 0; i < opacities.size(); i++) {
        CHECK(Math::is_equal_approx(opacities[i], expected));
    }
}

TEST_CASE("[GaussianSplatting][Animation] mixed numeric opacity keyframes interpolate linearly") {
    using namespace GaussianSplatting;

    GaussianAnimationStateMachine state_machine;
    int clip_index = state_machine.add_clip("mixed_numeric_opacity", 1.0f);
    state_machine.set_splat_count(1);

    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_OPACITY, 0.0f, 0);
    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_OPACITY, 1.0f, 2.0f);
    state_machine.play(clip_index);

    CHECK(Math::is_equal_approx(state_machine.sample_opacity(0, 0.5f), 1.0f));
}
TEST_CASE("[GaussianSplatting][Animation] state machine transitions") {
    using namespace GaussianSplatting;

    GaussianAnimationStateMachine state_machine;
    int clip_index = state_machine.add_clip("idle", 1.0f);
    state_machine.set_splat_count(1);

    CHECK_EQ(state_machine.get_state(), ANIMATION_STATE_STOPPED);

    state_machine.play(clip_index);
    CHECK_EQ(state_machine.get_state(), ANIMATION_STATE_PLAYING);

    state_machine.pause();
    CHECK_EQ(state_machine.get_state(), ANIMATION_STATE_PAUSED);

    state_machine.stop();
    CHECK_EQ(state_machine.get_state(), ANIMATION_STATE_STOPPED);

    state_machine.seek(0.25f);
    CHECK_EQ(state_machine.get_state(), ANIMATION_STATE_SEEKING);
}

TEST_CASE("[GaussianSplatting][Animation] cubic-bezier easing solves X-for-parameter (regression)") {
    using namespace GaussianSplatting;

    // Drive the private _cubic_bezier through the PUBLIC sampling path: an
    // OPACITY track with two CUBIC_BEZIER keyframes at t=0 (value a) and
    // t=1 (value b). For the segment between them the curve's control points
    // are keyframe-A's out_handle (p1) and keyframe-B's in_handle (p2), so
    // sample_opacity(0, t) == a + (b - a) * Y(s) where X(s) == t. With a=0,
    // b=1 the sampled value is exactly the easing curve's Y output.
    auto eval_bezier = [](const Vector2& p1, const Vector2& p2, float a, float b, float t) -> float {
        GaussianAnimationStateMachine sm;
        int clip = sm.add_clip("bezier", 1.0f);
        sm.set_splat_count(1);
        // add_keyframe_bezier(clip, prop, time, value, in_handle, out_handle):
        //   A.out_handle (6th arg) -> p1,  B.in_handle (5th arg) -> p2.
        sm.add_keyframe_bezier(clip, ANIMATION_PROPERTY_OPACITY, 0.0f, a, Vector2(0, 0), p1);
        sm.add_keyframe_bezier(clip, ANIMATION_PROPERTY_OPACITY, 1.0f, b, p2, Vector2(1, 1));
        sm.play(clip);
        return sm.sample_opacity(0, t);
    };

    // 1) IDENTITY/LINEAR: the diagonal handles produce a plain lerp.
    const Vector2 lin_p1(1.0f / 3.0f, 1.0f / 3.0f);
    const Vector2 lin_p2(2.0f / 3.0f, 2.0f / 3.0f);
    CHECK(Math::abs(eval_bezier(lin_p1, lin_p2, 0.0f, 1.0f, 0.25f) - 0.25f) < 1e-3f);
    CHECK(Math::abs(eval_bezier(lin_p1, lin_p2, 0.0f, 1.0f, 0.5f) - 0.5f) < 1e-3f);
    CHECK(Math::abs(eval_bezier(lin_p1, lin_p2, 0.0f, 1.0f, 0.75f) - 0.75f) < 1e-3f);

    // 2) ENDPOINTS map exactly to the keyframe values.
    CHECK(Math::is_equal_approx(eval_bezier(lin_p1, lin_p2, 2.0f, 8.0f, 0.0f), 2.0f));
    CHECK(Math::is_equal_approx(eval_bezier(lin_p1, lin_p2, 2.0f, 8.0f, 1.0f), 8.0f));

    // 3) EASE-IN (slow start): CSS cubic-bezier(0.42, 0, 1, 1). Because X must
    //    be solved for the curve parameter, y(0.5) ~= 0.315, well below the
    //    linear midpoint 0.5. The OLD buggy code evaluated Y(t) directly and,
    //    with p1.y=0 and p2.y=1, returned EXACTLY 0.5 at t=0.5 -- so the strict
    //    "< 0.45" check is the discriminating assertion that fails the old code.
    const Vector2 ease_p1(0.42f, 0.0f);
    const Vector2 ease_p2(1.0f, 1.0f);
    const float eased_mid = eval_bezier(ease_p1, ease_p2, 0.0f, 1.0f, 0.5f);
    CHECK(eased_mid < 0.45f);                     // discriminator: old code gives 0.5
    CHECK(Math::abs(eased_mid - 0.315f) < 0.03f); // matches the solved Y value

    // 4) MONOTONIC: an ease curve never decreases across the timeline.
    float prev = eval_bezier(ease_p1, ease_p2, 0.0f, 1.0f, 0.0f);
    for (int i = 1; i <= 10; i++) {
        const float t = static_cast<float>(i) / 10.0f;
        const float cur = eval_bezier(ease_p1, ease_p2, 0.0f, 1.0f, t);
        CHECK(cur >= prev - 1e-4f);
        prev = cur;
    }
}

#include "keyframe_interpolator.h"
#include "core/math/math_funcs.h"

namespace GaussianSplatting {

Variant KeyframeInterpolator::interpolate(const LocalVector<Keyframe>& keyframes, float time) const {
    if (keyframes.is_empty()) {
        return Variant();
    }

    if (keyframes.size() == 1) {
        return keyframes[0].value;
    }

    // Find surrounding keyframes
    int index_a, index_b;
    _find_keyframe_indices(keyframes, time, index_a, index_b);

    if (index_a == index_b) {
        return keyframes[index_a].value;
    }

    const Keyframe& kf_a = keyframes[index_a];
    const Keyframe& kf_b = keyframes[index_b];

    float time_diff = kf_b.time - kf_a.time;
    if (time_diff <= 0.0f) {
        return kf_a.value;
    }

    float t = (time - kf_a.time) / time_diff;
    t = CLAMP(t, 0.0f, 1.0f);

    // Handle different value types
    Variant::Type type = kf_a.value.get_type();
    Variant::Type other_type = kf_b.value.get_type();
    const bool numeric_pair = (type == Variant::FLOAT || type == Variant::INT) &&
            (other_type == Variant::FLOAT || other_type == Variant::INT);
    if (!numeric_pair && type != other_type) {
        // Type mismatch, return first keyframe.
        return kf_a.value;
    }

    InterpolationType interp_type = kf_a.interpolation;

    if (numeric_pair) {
        float a = kf_a.value.operator float();
        float b = kf_b.value.operator float();
        return _interpolate_float(a, b, t, interp_type, kf_a.out_handle, kf_b.in_handle);
    }

    switch (type) {
        case Variant::FLOAT: {
            float a = kf_a.value.operator float();
            float b = kf_b.value.operator float();
            return _interpolate_float(a, b, t, interp_type, kf_a.out_handle, kf_b.in_handle);
        }
        case Variant::INT: {
            float a = kf_a.value.operator float();
            float b = kf_b.value.operator float();
            return _interpolate_float(a, b, t, interp_type, kf_a.out_handle, kf_b.in_handle);
        }
        case Variant::VECTOR3: {
            Vector3 a = kf_a.value.operator Vector3();
            Vector3 b = kf_b.value.operator Vector3();
            return _interpolate_vector3(a, b, t, interp_type, kf_a.out_handle, kf_b.in_handle);
        }
        case Variant::COLOR: {
            Color a = kf_a.value.operator Color();
            Color b = kf_b.value.operator Color();
            return _interpolate_color(a, b, t, interp_type);
        }
        case Variant::QUATERNION: {
            Quaternion a = kf_a.value.operator Quaternion();
            Quaternion b = kf_b.value.operator Quaternion();
            return _interpolate_quaternion(a, b, t, interp_type);
        }
        default:
            // Unsupported type, return constant
            return kf_a.value;
    }
}

Vector3 KeyframeInterpolator::_interpolate_vector3(const Vector3& a, const Vector3& b, float t, InterpolationType type, const Vector2& handle_a, const Vector2& handle_b) {
    switch (type) {
        case InterpolationType::CONSTANT:
            return a;
        case InterpolationType::LINEAR:
            return a.lerp(b, t);
        case InterpolationType::CUBIC_BEZIER:
            return _cubic_bezier_vector3(t, a, b, handle_a, handle_b);
        case InterpolationType::SMOOTH_STEP:
            return a.lerp(b, smooth_step(t));
        case InterpolationType::SMOOTHER_STEP:
            return a.lerp(b, smoother_step(t));
        default:
            return a.lerp(b, t);
    }
}

Color KeyframeInterpolator::_interpolate_color(const Color& a, const Color& b, float t, InterpolationType type) {
    switch (type) {
        case InterpolationType::CONSTANT:
            return a;
        case InterpolationType::LINEAR:
            return a.lerp(b, t);
        case InterpolationType::SMOOTH_STEP:
            return a.lerp(b, smooth_step(t));
        case InterpolationType::SMOOTHER_STEP:
            return a.lerp(b, smoother_step(t));
        default:
            return a.lerp(b, t);
    }
}

Quaternion KeyframeInterpolator::_interpolate_quaternion(const Quaternion& a, const Quaternion& b, float t, InterpolationType type) {
    switch (type) {
        case InterpolationType::CONSTANT:
            return a;
        case InterpolationType::LINEAR:
            return a.slerp(b, t);
        case InterpolationType::SMOOTH_STEP:
            return a.slerp(b, smooth_step(t));
        case InterpolationType::SMOOTHER_STEP:
            return a.slerp(b, smoother_step(t));
        default:
            return a.slerp(b, t);
    }
}

float KeyframeInterpolator::_interpolate_float(float a, float b, float t, InterpolationType type, const Vector2& handle_a, const Vector2& handle_b) {
    switch (type) {
        case InterpolationType::CONSTANT:
            return a;
        case InterpolationType::LINEAR:
            return Math::lerp(a, b, t);
        case InterpolationType::CUBIC_BEZIER:
            return a + (b - a) * _cubic_bezier(t, handle_a, handle_b);
        case InterpolationType::SMOOTH_STEP:
            return Math::lerp(a, b, smooth_step(t));
        case InterpolationType::SMOOTHER_STEP:
            return Math::lerp(a, b, smoother_step(t));
        default:
            return Math::lerp(a, b, t);
    }
}

float KeyframeInterpolator::_cubic_bezier(float t, const Vector2& p1, const Vector2& p2) {
    // CSS-style cubic-bezier easing with FIXED endpoints P0=(0,0) and P3=(1,1)
    // and user control points P1=p1, P2=p2. The input `t` is the desired X
    // (normalized time), NOT the curve parameter. Contract: first SOLVE for the
    // curve parameter `s` such that X(s) == t, THEN return Y(s). Evaluating
    // Y(t) directly (the previous implementation) ignores p1.x/p2.x and yields
    // wrong easing for any non-default-X handle. Standard browser UnitBezier
    // approach: Newton-Raphson with a bisection fallback.
    if (t <= 0.0f) {
        return 0.0f;
    }
    if (t >= 1.0f) {
        return 1.0f;
    }

    // Polynomial-coefficient (Horner) form of the cubic with P0=(0,0), P3=(1,1):
    //   X(s) = ((ax*s + bx)*s + cx)*s,   dX/ds = (3*ax*s + 2*bx)*s + cx
    //   Y(s) = ((ay*s + by)*s + cy)*s
    const float cx = 3.0f * p1.x;
    const float bx = 3.0f * (p2.x - p1.x) - cx;
    const float ax = 1.0f - cx - bx;

    const float cy = 3.0f * p1.y;
    const float by = 3.0f * (p2.y - p1.y) - cy;
    const float ay = 1.0f - cy - by;

    auto sample_x = [ax, bx, cx](float s) -> float {
        return ((ax * s + bx) * s + cx) * s;
    };
    auto sample_y = [ay, by, cy](float s) -> float {
        return ((ay * s + by) * s + cy) * s;
    };
    auto dx_ds = [ax, bx, cx](float s) -> float {
        return (3.0f * ax * s + 2.0f * bx) * s + cx;
    };

    // Solve X(s) == t for s. Newton-Raphson from initial guess s = t.
    float s = t;
    bool converged = false;
    for (int i = 0; i < 8; i++) {
        const float x_err = sample_x(s) - t;
        if (Math::abs(x_err) < 1e-6f) {
            converged = true;
            break;
        }
        const float d = dx_ds(s);
        if (Math::abs(d) < 1e-6f) {
            break; // Derivative too small (divide-by-zero guard); use bisection.
        }
        s -= x_err / d;
    }

    // Bisection fallback if Newton failed to converge or wandered out of [0, 1].
    // X(s) is monotonic on [0, 1] for a valid easing curve, so this is safe.
    if (!converged || s < 0.0f || s > 1.0f) {
        float lo = 0.0f;
        float hi = 1.0f;
        s = t;
        for (int i = 0; i < 20; i++) {
            const float x = sample_x(s);
            if (Math::abs(x - t) < 1e-6f) {
                break;
            }
            if (x < t) {
                lo = s;
            } else {
                hi = s;
            }
            s = 0.5f * (lo + hi);
        }
    }

    return sample_y(s);
}

Vector3 KeyframeInterpolator::_cubic_bezier_vector3(float t, const Vector3& start, const Vector3& end, const Vector2& handle_a, const Vector2& handle_b) {
    Vector3 result;
    result.x = start.x + (end.x - start.x) * _cubic_bezier(t, handle_a, handle_b);
    result.y = start.y + (end.y - start.y) * _cubic_bezier(t, handle_a, handle_b);
    result.z = start.z + (end.z - start.z) * _cubic_bezier(t, handle_a, handle_b);
    return result;
}

void KeyframeInterpolator::_find_keyframe_indices(const LocalVector<Keyframe>& keyframes, float time, int& index_a, int& index_b) {
    int size = keyframes.size();

    if (size == 0) {
        index_a = index_b = 0;
        return;
    }

    if (size == 1 || time <= keyframes[0].time) {
        index_a = index_b = 0;
        return;
    }

    if (time >= keyframes[size - 1].time) {
        index_a = index_b = size - 1;
        return;
    }

    // Binary search for efficiency
    int left = 0;
    int right = size - 1;

    while (left < right - 1) {
        int mid = (left + right) / 2;
        if (keyframes[mid].time <= time) {
            left = mid;
        } else {
            right = mid;
        }
    }

    index_a = left;
    index_b = right;
}

float KeyframeInterpolator::smooth_step(float t) {
    return t * t * (3.0f - 2.0f * t);
}

float KeyframeInterpolator::smoother_step(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

int KeyframeInterpolator::add_keyframe_sorted(LocalVector<Keyframe>& keyframes, const Keyframe& keyframe) {
    // Find insertion point
    uint32_t insertion_point = 0;
    for (uint32_t i = 0; i < keyframes.size(); i++) {
        if (keyframes[i].time > keyframe.time) {
            insertion_point = i;
            break;
        }
        insertion_point = i + 1;
    }

    keyframes.insert(insertion_point, keyframe);
    return insertion_point;
}

} // namespace GaussianSplatting

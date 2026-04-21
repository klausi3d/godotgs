/**
 * gaussian_data_animation.cpp -- Companion .cpp for gaussian_data.h
 *
 * Contains the animation system integration methods of GaussianData:
 *   - Animation state machine / incremental saver wiring
 *   - Per-frame animation update and cache management
 *   - Per-splat animated property accessors (position, color, opacity,
 *     scale, rotation)
 */

#include "gaussian_data.h"

// ---------------------------------------------------------------------------
// Animation state-machine & incremental-saver wiring
// ---------------------------------------------------------------------------

void GaussianData::set_animation_state_machine(const Ref<GaussianSplatting::GaussianAnimationStateMachine>& p_animation) {
    animation_state_machine = p_animation;
    if (animation_state_machine.is_valid()) {
        animation_state_machine->set_splat_count(gaussians.size());
        if (incremental_saver.is_valid()) {
            animation_state_machine->set_incremental_saver(incremental_saver);
        }
    }
    {
        MutexLock anim_lock(animation_cache_mutex);
        animation_cache_dirty = true;
    }
}

void GaussianData::set_incremental_saver(const Ref<GaussianSplatting::GaussianIncrementalSaver>& p_saver) {
    incremental_saver = p_saver;
    if (incremental_saver.is_valid() && animation_state_machine.is_valid()) {
        animation_state_machine->set_incremental_saver(incremental_saver);
    }
}

// ---------------------------------------------------------------------------
// Per-frame animation update
// ---------------------------------------------------------------------------

void GaussianData::update_animation(float p_delta) {
    if (!animation_state_machine.is_valid() || !animation_enabled) {
        return;
    }

    animation_state_machine->update(p_delta);
    {
        MutexLock anim_lock(animation_cache_mutex);
        animation_cache_dirty = true;
    }
}

// ---------------------------------------------------------------------------
// Non-destructive animation view.
//
// The prior apply_animation_at_time() destructively wrote sampled animation
// values back into the base Gaussian payload. That collapsed the distinction
// between base state and the animated view and made GaussianData's mutable
// runtime truth time-dependent. It has been removed -- animation is now
// strictly a sampling view, exposed through the per-property accessors
// below. Consumers that want a baked pose must construct a new GaussianData
// and stamp the sampled values into it themselves.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Per-splat animated property accessors
// ---------------------------------------------------------------------------

Vector3 GaussianData::get_animated_position(int p_index, float p_time) const {
    if (p_index < 0 || (uint32_t)p_index >= gaussians.size()) {
        return Vector3();
    }

    if (!animation_state_machine.is_valid() || !animation_enabled) {
        return gaussians[p_index].position;
    }

    Vector3 animated_pos;
    if (animation_state_machine->try_sample_position(p_index, p_time, animated_pos)) {
        return animated_pos;
    }

    return gaussians[p_index].position;
}

Color GaussianData::get_animated_color(int p_index, float p_time) const {
    if (p_index < 0 || (uint32_t)p_index >= gaussians.size()) {
        return Color();
    }

    if (!animation_state_machine.is_valid() || !animation_enabled) {
        return gaussians[p_index].sh_dc;
    }

    Color animated_color;
    if (animation_state_machine->try_sample_color(p_index, p_time, animated_color)) {
        return animated_color;
    }

    return gaussians[p_index].sh_dc;
}

float GaussianData::get_animated_opacity(int p_index, float p_time) const {
    if (p_index < 0 || (uint32_t)p_index >= gaussians.size()) {
        return 1.0f;
    }

    if (!animation_state_machine.is_valid() || !animation_enabled) {
        return gaussians[p_index].opacity;
    }

    float animated_opacity;
    if (animation_state_machine->try_sample_opacity(p_index, p_time, animated_opacity)) {
        return animated_opacity;
    }

    return gaussians[p_index].opacity;
}

Vector3 GaussianData::get_animated_scale(int p_index, float p_time) const {
    if (p_index < 0 || (uint32_t)p_index >= gaussians.size()) {
        return Vector3(1, 1, 1);
    }

    if (!animation_state_machine.is_valid() || !animation_enabled) {
        return gaussians[p_index].scale;
    }

    Vector3 animated_scale;
    if (animation_state_machine->try_sample_scale(p_index, p_time, animated_scale)) {
        return animated_scale;
    }

    return gaussians[p_index].scale;
}

Quaternion GaussianData::get_animated_rotation(int p_index, float p_time) const {
    if (p_index < 0 || (uint32_t)p_index >= gaussians.size()) {
        return Quaternion();
    }

    if (!animation_state_machine.is_valid() || !animation_enabled) {
        return gaussians[p_index].rotation;
    }

    Quaternion animated_rotation;
    if (animation_state_machine->try_sample_rotation(p_index, p_time, animated_rotation)) {
        return animated_rotation;
    }

    return gaussians[p_index].rotation;
}

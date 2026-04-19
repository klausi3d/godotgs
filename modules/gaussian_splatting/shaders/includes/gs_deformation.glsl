#ifndef GS_DEFORMATION_GLSL
#define GS_DEFORMATION_GLSL

// Hash an instance identifier for wind phase variation.
uint gs_wind_hash_u32(uint v) {
    v ^= v >> 16u;
    v *= 0x7feb352du;
    v ^= v >> 15u;
    v *= 0x846ca68bu;
    v ^= v >> 16u;
    return v;
}

// Decode the per-instance wind mode from the packed float value.
uint gs_decode_instance_wind_mode(float encoded_mode) {
    return uint(clamp(floor(encoded_mode + 0.5), 0.0, float(GS_INSTANCE_WIND_MODE_FORCE_ENABLED)));
}

struct GSDeformationResult {
    vec3 position;
    float opacity;
};

// Check whether wind deformation is enabled for the current instance.
bool gs_is_wind_enabled_for_instance(float encoded_mode, vec4 wind_time_config) {
    bool wind_enabled = wind_time_config.w > 0.5;
    uint mode = gs_decode_instance_wind_mode(encoded_mode);
    if (mode == GS_INSTANCE_WIND_MODE_FORCE_ENABLED) {
        return true;
    }
    if (mode == GS_INSTANCE_WIND_MODE_FORCE_DISABLED) {
        return false;
    }
    return wind_enabled;
}

float gs_compute_sphere_effector_weight(vec3 world_position, vec4 effector_sphere, vec4 effector_config,
        float time_seconds, uint stable_seed, out vec3 out_direction) {
    out_direction = vec3(0.0, 1.0, 0.0);
    if (effector_config.x <= 0.5) {
        return 0.0;
    }

    float radius = effector_sphere.w;
    if (radius <= 1e-6) {
        return 0.0;
    }

    vec3 delta = world_position - effector_sphere.xyz;
    float distance_to_center = length(delta);
    if (distance_to_center >= radius) {
        return 0.0;
    }

    float normalized = clamp(1.0 - (distance_to_center / radius), 0.0, 1.0);
    float falloff = max(effector_config.z, 0.001);
    float influence = pow(normalized, falloff);
    float anim_freq = effector_config.w > 0.0 ? effector_config.w : 2.0;
    uint hashed = gs_wind_hash_u32(stable_seed);
    float phase_jitter = (float(hashed & 0xFFFFu) / 65535.0) * 6.28318530718;
    float anim_phase = time_seconds * anim_freq * 6.28318530718 + phase_jitter;
    float anim_factor = 0.5 + 0.5 * sin(anim_phase);

    if (distance_to_center > 1e-6) {
        out_direction = delta / distance_to_center;
    }
    return influence * anim_factor;
}

GSDeformationResult gs_apply_sphere_effector(vec3 world_position,
        float base_opacity,
        vec4 instance_effect_config,
        vec4 effector_sphere,
        vec4 effector_config,
        vec4 effector_opacity_config,
        float time_seconds,
        uint stable_seed) {
    GSDeformationResult result;
    result.position = world_position;
    result.opacity = clamp(base_opacity, 0.0, 1.0);

    vec3 direction;
    float weight = gs_compute_sphere_effector_weight(world_position, effector_sphere, effector_config,
            time_seconds, stable_seed, direction);
    if (weight <= 0.0) {
        return result;
    }

    float position_scale = max(instance_effect_config.x, 0.0);
    float strength = effector_config.y;
    if (effector_opacity_config.x > 0.5 && position_scale > 0.0 && abs(strength) > 1e-8) {
        result.position += direction * (strength * weight * position_scale);
    }

    if (effector_opacity_config.y > 0.5) {
        float opacity_scale = max(instance_effect_config.y, 0.0);
        float opacity_strength = clamp(effector_opacity_config.z, 0.0, 1.0) * opacity_scale;
        if (opacity_strength > 0.0) {
            float target_opacity = clamp(effector_opacity_config.w, 0.0, 1.0);
            float opacity_weight = clamp(weight * opacity_strength, 0.0, 1.0);
            result.opacity = clamp(mix(result.opacity, target_opacity, opacity_weight), 0.0, 1.0);
        }
    }

    return result;
}

GSDeformationResult gs_apply_wind_deformation(vec3 world_position,
        uint stable_seed,
        float opacity,
        float instance_intensity,
        float instance_wind_mode,
        vec4 instance_wind_config,
        vec4 instance_effect_config,
        vec4 wind_dir_strength,
        vec4 wind_time_config,
        vec4 effector_sphere,
        vec4 effector_config,
        vec4 effector_opacity_config) {
    float time_seconds = wind_time_config.x;
    bool wind_enabled = gs_is_wind_enabled_for_instance(instance_wind_mode, wind_time_config);
    if (!wind_enabled) {
        return gs_apply_sphere_effector(world_position, opacity, instance_effect_config, effector_sphere,
                effector_config, effector_opacity_config, time_seconds, stable_seed);
    }

    vec3 direction = wind_dir_strength.xyz;
    if (dot(instance_wind_config.xyz, instance_wind_config.xyz) > 1e-8) {
        direction = instance_wind_config.xyz;
    }
    float direction_len = length(direction);
    float strength = max(wind_dir_strength.w, 0.0);
    if (direction_len <= 1e-6 || strength <= 0.0) {
        return gs_apply_sphere_effector(world_position, opacity, instance_effect_config, effector_sphere,
                effector_config, effector_opacity_config, time_seconds, stable_seed);
    }

    float instance_frequency_scale = instance_wind_config.w > 0.0 ? instance_wind_config.w : 1.0;
    float temporal_frequency = max(wind_time_config.y, 0.0) * instance_frequency_scale;
    float spatial_frequency = wind_time_config.z;
    float clamped_intensity = max(instance_intensity, 0.0);
    if (clamped_intensity <= 0.0) {
        return gs_apply_sphere_effector(world_position, opacity, instance_effect_config, effector_sphere,
                effector_config, effector_opacity_config, time_seconds, stable_seed);
    }

    // Reuse opacity as a rough "stiffness" signal until a dedicated attribute exists.
    float opacity_resistance = clamp(1.0 - opacity, 0.0, 1.0);
    float resistance = mix(0.2, 1.0, opacity_resistance);

    uint hashed = gs_wind_hash_u32(stable_seed);
    float phase_jitter = (float(hashed & 0xFFFFu) / 65535.0) * 6.28318530718;
    float phase = dot(world_position.xz, vec2(spatial_frequency)) + time_seconds * temporal_frequency + phase_jitter;
    float displacement = sin(phase) * strength * resistance * clamped_intensity;

    vec3 deformed = world_position + (direction / direction_len) * displacement;
    return gs_apply_sphere_effector(deformed, opacity, instance_effect_config, effector_sphere,
            effector_config, effector_opacity_config, time_seconds, stable_seed);
}

#endif // GS_DEFORMATION_GLSL

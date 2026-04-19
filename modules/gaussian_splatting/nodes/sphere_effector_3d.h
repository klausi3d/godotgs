/**
 * @file sphere_effector_3d.h
 * @brief Scene-driven sphere effector node for Gaussian splat deformation.
 */

#ifndef SPHERE_EFFECTOR_3D_H
#define SPHERE_EFFECTOR_3D_H

#include "core/string/node_path.h"
#include "scene/3d/node_3d.h"

/**
 * @class SphereEffector3D
 * @brief Authorable scene node that describes one sphere effector for Gaussian splat effects.
 *
 * The node only owns authoring/state for now. Scene-director integration and GPU
 * binding are handled elsewhere so the node can stay serialization-safe and editor-friendly.
 */
class SphereEffector3D : public Node3D {
    GDCLASS(SphereEffector3D, Node3D);

public:
    enum ScopeMode {
        SCOPE_WORLD = 0,
        SCOPE_SUBTREE = 1,
        SCOPE_EXPLICIT_ROOT = 2,
    };

private:
    bool enabled = false;
    float radius = 0.0f;
    float strength = 0.0f;
    float falloff = 2.0f;
    float frequency = 2.0f;
    bool affect_position = true;
    bool affect_opacity = false;
    float opacity_strength = 1.0f;
    float target_opacity = 0.0f;
    uint32_t layer_mask = 1u;
    int scope_mode = SCOPE_SUBTREE;
    NodePath scope_root;
    int priority = 0;

protected:
    static void _bind_methods();
    void _validate_property(PropertyInfo &p_property) const;
    void _notification(int p_what);

private:
    // Push (or refresh) this effector's state into the scene director. Called
    // on ENTER_TREE, TRANSFORM_CHANGED, and every property setter so the
    // director's cached list stays fresh without the render thread ever
    // having to walk the scene tree.
    void _sync_with_director();
    void _unregister_from_director();

public:
    SphereEffector3D();
    ~SphereEffector3D() override = default;

    void set_enabled(bool p_enabled);
    bool is_enabled() const { return enabled; }

    void set_radius(float p_radius);
    float get_radius() const { return radius; }

    void set_strength(float p_strength);
    float get_strength() const { return strength; }

    void set_falloff(float p_falloff);
    float get_falloff() const { return falloff; }

    void set_frequency(float p_frequency);
    float get_frequency() const { return frequency; }

    void set_affect_position(bool p_affect_position);
    bool is_affecting_position() const { return affect_position; }

    void set_affect_opacity(bool p_affect_opacity);
    bool is_affecting_opacity() const { return affect_opacity; }

    void set_opacity_strength(float p_opacity_strength);
    float get_opacity_strength() const { return opacity_strength; }

    void set_target_opacity(float p_target_opacity);
    float get_target_opacity() const { return target_opacity; }

    void set_layer_mask(uint32_t p_layer_mask);
    uint32_t get_layer_mask() const { return layer_mask; }

    void set_scope_mode(int p_scope_mode);
    int get_scope_mode() const { return scope_mode; }

    void set_scope_root(const NodePath &p_scope_root);
    NodePath get_scope_root() const { return scope_root; }

    void set_priority(int p_priority);
    int get_priority() const { return priority; }

    PackedStringArray get_configuration_warnings() const override;
};

VARIANT_ENUM_CAST(SphereEffector3D::ScopeMode);

#endif // SPHERE_EFFECTOR_3D_H

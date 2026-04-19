#ifndef GAUSSIAN_SPLAT_SCENE_DIRECTOR_H
#define GAUSSIAN_SPLAT_SCENE_DIRECTOR_H

#include "core/object/object.h"
#include "core/object/object_id.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"
#include "core/math/transform_3d.h"
#include "core/math/aabb.h"
#include "core/variant/variant.h"
#include "scene/resources/3d/world_3d.h"

#include "gaussian_data.h"
#include "gaussian_splat_asset.h"
#include "streaming_chunk_payload_source.h"
#include "../lod/lod_config.h"
#include "../renderer/gaussian_splat_renderer.h"

class ColorGradingResource;
class Node;
class Node3D;

class GaussianSplatSceneDirector : public Object {
    GDCLASS(GaussianSplatSceneDirector, Object);

public:
    enum InstanceWindMode : uint32_t {
        INSTANCE_WIND_INHERIT = 0u,
        INSTANCE_WIND_FORCE_DISABLED = 1u,
        INSTANCE_WIND_FORCE_ENABLED = 2u,
    };

    enum SubmissionResidencyHint : int32_t {
        SUBMISSION_RESIDENCY_HINT_RESIDENT = 0,
        SUBMISSION_RESIDENCY_HINT_STREAMING = 1,
    };

    struct InstanceSubmission {
        ObjectID node_id;
        RID scenario;
        Ref<GaussianSplatRenderer> renderer;
        Ref<GaussianSplatAsset> asset;
        Transform3D transform;
        float opacity = 1.0f;
        float lod_bias = 0.0f;
        float wind_intensity = 1.0f;
        uint32_t wind_mode = INSTANCE_WIND_INHERIT;
        Vector3 wind_direction = Vector3();
        float wind_frequency = 1.0f;
        float effect_position_scale = 1.0f;
        float effect_opacity_scale = 1.0f;
        uint32_t flags = 0;
        uint32_t last_lod = 0;
        bool casts_shadow = false;
        bool visible = true;
        bool has_desired_residency_hint = false;
        int32_t desired_residency_hint = SUBMISSION_RESIDENCY_HINT_RESIDENT;
        Ref<ColorGradingResource> color_grading;
    };

    struct WorldSubmission {
        ObjectID owner_id;
        RID scenario;
        Ref<GaussianData> gaussian_data;
        Ref<ChunkPayloadSource> payload_source;
        Vector<GaussianSplatRenderer::StaticChunk> static_chunks;
        AABB bounds;
        Dictionary metadata;
        bool has_desired_residency_hint = false;
        int32_t desired_residency_hint = SUBMISSION_RESIDENCY_HINT_RESIDENT;
        Dictionary desired_renderer_overrides;
    };

    struct SubmissionCounts {
        uint32_t instance_submissions = 0;
        uint32_t world_submissions = 0;
    };

    enum SphereEffectorScopeMode : uint32_t {
        SPHERE_EFFECTOR_SCOPE_WORLD = 0u,
        SPHERE_EFFECTOR_SCOPE_SUBTREE = 1u,
        SPHERE_EFFECTOR_SCOPE_EXPLICIT_ROOT = 2u,
    };

    struct SphereEffectorSelection {
        ObjectID effector_id;
        RID scenario;
        Transform3D transform;
        Vector3 center;
        float radius = 0.0f;
        float strength = 0.0f;
        float falloff = 2.0f;
        float frequency = 2.0f;
        float opacity_strength = 1.0f;
        uint32_t layer_mask = 1u;
        uint32_t scope_mode = SPHERE_EFFECTOR_SCOPE_SUBTREE;
        ObjectID scope_root_id;
        int32_t priority = 0;
        uint32_t matched_effector_count = 0;
        bool enabled = false;
        bool affect_position = true;
        bool affect_opacity = false;
    };

    static GaussianSplatSceneDirector *get_singleton();

    GaussianSplatSceneDirector();
    ~GaussianSplatSceneDirector();

	void register_instance(ObjectID p_node_id, const Ref<GaussianSplatAsset> &p_asset, const Transform3D &p_transform,
			float p_opacity, float p_lod_bias, uint32_t p_flags, bool p_casts_shadow = false,
			float p_wind_intensity = 1.0f, uint32_t p_wind_mode = INSTANCE_WIND_INHERIT,
			const Vector3 &p_wind_direction = Vector3(), float p_wind_frequency = 1.0f,
			bool p_visible = true, bool p_has_desired_residency_hint = false,
			int32_t p_desired_residency_hint = SUBMISSION_RESIDENCY_HINT_RESIDENT,
			float p_effect_position_scale = 1.0f, float p_effect_opacity_scale = 1.0f);
	void update_instance_transform(ObjectID p_node_id, const Transform3D &p_transform);
	// Cache the per-instance scene-effector filter state on the InstanceRecord. Called
	// from GaussianSplatNode3D setters so the render thread never has to read these
	// fields back from the live Node3D during build_instance_buffer_for_renderer.
	void update_instance_scene_effector_filter(ObjectID p_node_id, bool p_enabled,
			uint32_t p_layer_mask, bool p_scope_filter_present, bool p_scope_filter_valid,
			ObjectID p_scope_root_id, const LocalVector<ObjectID> &p_scene_tree_ancestor_ids);
	void update_instance_params(ObjectID p_node_id, float p_opacity, float p_lod_bias, uint32_t p_flags, bool p_casts_shadow = false,
			float p_wind_intensity = 1.0f, uint32_t p_wind_mode = INSTANCE_WIND_INHERIT,
			const Vector3 &p_wind_direction = Vector3(), float p_wind_frequency = 1.0f,
			bool p_visible = true, bool p_has_desired_residency_hint = false,
			int32_t p_desired_residency_hint = SUBMISSION_RESIDENCY_HINT_RESIDENT,
			float p_effect_position_scale = 1.0f, float p_effect_opacity_scale = 1.0f);
	void unregister_instance(ObjectID p_node_id);
	void update_instance_lods(const Vector3 &p_camera_pos, const LODConfig &p_lod_config, float p_hysteresis_zone);
    void update_instance_lods_for_renderer(const GaussianSplatRenderer *p_renderer, const Vector3 &p_camera_pos,
            const LODConfig &p_lod_config, float p_hysteresis_zone);
    void build_instance_buffer(LocalVector<InstanceDataGPU> &out) const;
	void build_instance_buffer_for_renderer(const GaussianSplatRenderer *p_renderer, LocalVector<InstanceDataGPU> &out,
			bool p_shadow_casters_only = false) const;
	// Build the per-instance color grading SSBO for the supplied renderer. Walks the same
	// instance list as build_instance_buffer_for_renderer; falls back to the renderer's
	// RenderConfig::color_grading when a record has no per-instance ref. When no director
	// instances exist but the renderer is active (early setup / legacy world-submission shim),
	// produces a 1-row buffer so the shader always has a valid index.
	void build_instance_grading_buffer_for_renderer(const GaussianSplatRenderer *p_renderer,
			LocalVector<InstanceGradingGPU> &out, bool p_shadow_casters_only = false) const;
	// Fill a single GPU grading row from a ColorGradingResource ref (null → neutral
	// disabled). Exposed so streaming/resident fallback paths that inject synthetic
	// instance rows outside the director's record list can still honor the renderer's
	// color_grading default instead of forcing neutral.
	static void fill_instance_grading_entry(const Ref<ColorGradingResource> &p_grading,
			InstanceGradingGPU &r_entry);
	// Per-instance color grading setter. Stores the grading ref on the record identified
	// by node_id; the next frame's build_instance_grading_buffer_for_renderer picks it up.
	// No-op when the node is unregistered.
	//
	// `p_force_refresh` controls cache-invalidation cadence. Callers that know the
	// underlying grading values just changed (e.g. a ColorGradingResource `changed`
	// signal for slider edits) pass true — the generation is bumped even when the
	// ref is unchanged so the buffer re-uploads with fresh values. Callers that
	// merely echo the current ref (per-frame apply) leave it false so unrelated
	// setting churn does not bust sort/raster caches every frame.
	bool update_instance_color_grading(ObjectID p_node_id, const Ref<ColorGradingResource> &p_grading,
			bool p_force_refresh = false);
	// Accessor for tests and diagnostics.
	Ref<ColorGradingResource> get_instance_color_grading(ObjectID p_node_id) const;
	// Bump the instance generation of the world bound to this renderer so the
	// next frame rebuilds the grading SSBO. Called when the renderer's legacy
	// renderer-wide color_grading default changes — records with no per-instance
	// grading read from that default via the build step's fallback, so their
	// rows need to re-upload even though no per-instance ref changed.
	void invalidate_grading_for_renderer(const GaussianSplatRenderer *p_renderer);
	// Hash every per-instance grading bound to this renderer. Used by the sort/raster cache
	// invalidation path so any node's grading edit busts the cache.
	//
	// `p_shadow_casters_only` mirrors the filter in build_instance_grading_buffer_for_renderer.
	// When the renderer is rendering a shadow pass, non-shadow-caster records are filtered
	// out of the grading buffer — their gradings MUST not participate in the shadow cache
	// signature either, otherwise grading edits on non-shadow nodes spuriously bust the
	// shadow sort/raster cache.
	uint64_t compute_color_grading_signature_for_renderer(const GaussianSplatRenderer *p_renderer,
			bool p_shadow_casters_only = false) const;
	uint32_t get_instance_count_for_renderer(const GaussianSplatRenderer *p_renderer) const;
	uint64_t get_instance_generation_for_renderer(const GaussianSplatRenderer *p_renderer) const;
    uint64_t get_instance_asset_generation_for_renderer(const GaussianSplatRenderer *p_renderer) const;
    void register_sphere_effector(ObjectID p_effector_id, const Transform3D &p_transform,
            float p_radius, float p_strength, float p_falloff, float p_frequency,
            bool p_enabled = true, bool p_affect_position = true, bool p_affect_opacity = false,
            float p_opacity_strength = 1.0f, uint32_t p_layer_mask = 1u,
            uint32_t p_scope_mode = SPHERE_EFFECTOR_SCOPE_SUBTREE,
            ObjectID p_scope_root_id = ObjectID(), int32_t p_priority = 0);
    void update_sphere_effector(ObjectID p_effector_id, const Transform3D &p_transform,
            float p_radius, float p_strength, float p_falloff, float p_frequency,
            bool p_enabled = true, bool p_affect_position = true, bool p_affect_opacity = false,
            float p_opacity_strength = 1.0f, uint32_t p_layer_mask = 1u,
            uint32_t p_scope_mode = SPHERE_EFFECTOR_SCOPE_SUBTREE,
            ObjectID p_scope_root_id = ObjectID(), int32_t p_priority = 0);
    void unregister_sphere_effector(ObjectID p_effector_id);
    // Build the renderer's effector payload. When `r_total_scene_effectors` is
    // non-null, it also returns the raw effector count for this renderer's
    // world under the same `world_mutex` lock — callers that need both values
    // should use this overload to avoid a double-query race where the main
    // thread can mutate the effector list between two director calls.
    void build_sphere_effector_payload_for_renderer(const GaussianSplatRenderer *p_renderer,
            LocalVector<SphereEffectorSelection> &out,
            uint32_t *r_total_scene_effectors = nullptr) const;
    bool get_primary_sphere_effector_for_instance(ObjectID p_node_id, SphereEffectorSelection *r_selection) const;
    uint32_t get_sphere_effector_count_for_renderer(const GaussianSplatRenderer *p_renderer) const;
    uint64_t get_sphere_effector_generation_for_renderer(const GaussianSplatRenderer *p_renderer) const;
    void register_instance_submission(ObjectID p_node_id, const Ref<GaussianSplatAsset> &p_asset,
            const Transform3D &p_transform, float p_opacity, float p_lod_bias, uint32_t p_flags,
            bool p_casts_shadow = false, float p_wind_intensity = 1.0f,
            uint32_t p_wind_mode = INSTANCE_WIND_INHERIT, const Vector3 &p_wind_direction = Vector3(),
            float p_wind_frequency = 1.0f, bool p_visible = true,
            bool p_has_desired_residency_hint = false,
            int32_t p_desired_residency_hint = SUBMISSION_RESIDENCY_HINT_RESIDENT,
            float p_effect_position_scale = 1.0f, float p_effect_opacity_scale = 1.0f);
    void update_instance_submission_transform(ObjectID p_node_id, const Transform3D &p_transform);
    void update_instance_submission_params(ObjectID p_node_id, float p_opacity, float p_lod_bias, uint32_t p_flags,
            bool p_casts_shadow = false, float p_wind_intensity = 1.0f,
            uint32_t p_wind_mode = INSTANCE_WIND_INHERIT, const Vector3 &p_wind_direction = Vector3(),
            float p_wind_frequency = 1.0f, bool p_visible = true,
            bool p_has_desired_residency_hint = false,
            int32_t p_desired_residency_hint = SUBMISSION_RESIDENCY_HINT_RESIDENT,
            float p_effect_position_scale = 1.0f, float p_effect_opacity_scale = 1.0f);
    void unregister_instance_submission(ObjectID p_node_id);
    bool get_instance_submission(ObjectID p_node_id, InstanceSubmission *r_submission) const;

	void collect_instance_assets_for_renderer(const GaussianSplatRenderer *p_renderer, LocalVector<InstanceAssetRegistration> &out,
			bool p_shadow_casters_only = false) const;
    // Runtime world-submission path. Applies the submitted payload to the shared renderer and
    // becomes the authoritative active world-backed source for the scenario.
    bool submit_world_submission(const WorldSubmission &p_submission);
    // Runtime inverse of submit_world_submission(). Clears renderer-owned world state and
    // releases the active world-backed source for this owner.
    void release_world_submission(ObjectID p_owner_id);
    bool get_world_submission(ObjectID p_owner_id, WorldSubmission *r_submission) const;
    bool get_world_submission_for_scenario(const RID &p_scenario, WorldSubmission *r_submission) const;
    bool has_world_submission_for_renderer(const GaussianSplatRenderer *p_renderer) const;
    // Current hint precedence is active world > homogeneous instance submissions.
    // Conflicting instance submission hints return false with source "mixed_instance_submissions".
    bool get_submission_residency_hint_for_renderer(const GaussianSplatRenderer *p_renderer,
            int32_t *r_hint, String *r_source = nullptr) const;
    SubmissionCounts get_submission_counts() const;

    Ref<GaussianSplatRenderer> get_shared_renderer(World3D *p_world);

protected:
    static void _bind_methods();

private:
    struct InstanceRecord {
        ObjectID node_id;
        Transform3D transform;
        float opacity = 1.0f;
        float lod_bias = 0.0f;
        float wind_intensity = 1.0f;
        uint32_t wind_mode = INSTANCE_WIND_INHERIT;
		Vector3 wind_direction = Vector3();
		float wind_frequency = 1.0f;
		float effect_position_scale = 1.0f;
		float effect_opacity_scale = 1.0f;
		uint32_t asset_id = 0;
		uint32_t flags = 0;
        uint32_t last_lod = 0;
        bool casts_shadow = false;
        bool visible = true;
        bool has_desired_residency_hint = false;
        int32_t desired_residency_hint = SUBMISSION_RESIDENCY_HINT_RESIDENT;
        bool dirty = true;
        Ref<ColorGradingResource> color_grading;

        // Scene-effector filter state — cached on registration / setters so the
        // render thread never has to read these from the live Node3D.
        bool scene_effectors_enabled = true;
        uint32_t scene_effector_layer_mask = 1u;
        bool scene_effector_scope_filter_present = false;
        bool scene_effector_scope_filter_valid = true;
        ObjectID scene_effector_scope_root_id;
        // Ancestor ObjectIDs (self + parents up to the scene root), cached at
        // registration/parent-change on the main thread. The render thread
        // reads this set to evaluate subtree containment for SCOPE_SUBTREE /
        // SCOPE_EXPLICIT_ROOT effectors without ever touching the live tree.
        LocalVector<ObjectID> scene_tree_ancestor_ids;
	};

    struct SphereEffectorRecord {
        ObjectID effector_id;
        Transform3D transform;
        float radius = 0.0f;
        float strength = 0.0f;
        float falloff = 2.0f;
        float frequency = 2.0f;
        float opacity_strength = 1.0f;
        uint32_t layer_mask = 1u;
        uint32_t scope_mode = SPHERE_EFFECTOR_SCOPE_SUBTREE;
        ObjectID scope_root_id;
        int32_t priority = 0;
        uint64_t registration_serial = 0;
        // Cached liveness of scope_root_id. Starts true on register, flipped
        // false (and triggers a generation bump) by the payload builder when
        // `ObjectDB::get_instance(scope_root_id)` no longer resolves.
        bool scope_root_valid = true;
        bool enabled = true;
        bool affect_position = true;
        bool affect_opacity = false;
    };

    struct SharedWorld {
        RID scenario;
        Ref<GaussianSplatRenderer> renderer;
        LocalVector<InstanceRecord> instances;
        HashMap<ObjectID, uint32_t> instance_lookup;
        uint64_t instance_generation = 1;
        uint64_t instance_asset_generation = 1;
        LocalVector<SphereEffectorRecord> sphere_effectors;
        HashMap<ObjectID, uint32_t> sphere_effector_lookup;
        uint64_t sphere_effector_generation = 1;
        uint64_t sphere_effector_registration_serial = 0;
	        struct WorldSubmissionRecord {
	            ObjectID owner_id;
	            Ref<GaussianData> gaussian_data;
	            Ref<ChunkPayloadSource> payload_source;
	            Vector<GaussianSplatRenderer::StaticChunk> static_chunks;
	            AABB bounds;
	            Dictionary metadata;
	            bool has_desired_residency_hint = false;
	            int32_t desired_residency_hint = SUBMISSION_RESIDENCY_HINT_RESIDENT;
	            Dictionary desired_renderer_overrides;
	            GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot renderer_restore_state;
	            bool active = false;
	        };
        WorldSubmissionRecord world_submission;
        struct AssetRecord {
            Ref<GaussianSplatAsset> asset;
            Ref<GaussianData> data;
            uint32_t refcount = 0;
            uint32_t edited_version = 0;
        };
        HashMap<uint32_t, AssetRecord> asset_records;
    };

    static GaussianSplatSceneDirector *singleton;

    mutable Mutex world_mutex;
    HashMap<RID, SharedWorld> worlds;
    mutable HashSet<ObjectID> scene_effector_multi_match_warned_nodes;

    SharedWorld *_get_or_create_world_for_scenario(const RID &p_scenario, bool p_require_renderer = true);
    SharedWorld *_get_or_create_world(World3D *p_world, bool p_require_renderer = true);
    SharedWorld *_get_world_for_instance(ObjectID p_node_id);
    SharedWorld *_find_world_for_instance(ObjectID p_node_id);
    SharedWorld *_get_world_for_effector(ObjectID p_effector_id);
    SharedWorld *_find_world_for_effector(ObjectID p_effector_id);
    SharedWorld *_find_world_for_renderer(const GaussianSplatRenderer *p_renderer);
    const SharedWorld *_find_world_for_renderer(const GaussianSplatRenderer *p_renderer) const;
    SharedWorld *_find_world_for_world_submission(ObjectID p_owner_id);
    const SharedWorld *_find_world_for_world_submission(ObjectID p_owner_id) const;
    static void _build_sorted_sphere_effector_payload(const SharedWorld &p_world,
            LocalVector<SphereEffectorSelection> &r_out);

    // Render-thread-safe mask builder: consumes the scene-effector filter state
    // cached on the InstanceRecord instead of reading it back from the live
    // Node3D. The main-thread node path keeps the cache fresh via
    // update_instance_scene_effector_filter(). This path skips the implicit-
    // subtree ancestor check because `is_ancestor_of` is a scene-tree walk and
    // is not safe to call from the render thread; use SCOPE_EXPLICIT_ROOT +
    // `rendering/scene_effector_scope_root` on the node to opt into scoping.
    static uint32_t _build_scene_effector_mask_for_record(const InstanceRecord &p_record,
            const LocalVector<SphereEffectorSelection> &p_payload);

    static bool _populate_gaussian_data_from_asset(const Ref<GaussianSplatAsset> &p_asset, Ref<GaussianData> &r_data);
    static bool _retain_asset_record(SharedWorld &p_world, const Ref<GaussianSplatAsset> &p_asset, uint32_t p_asset_id);
    static bool _refresh_asset_record(SharedWorld &p_world, const Ref<GaussianSplatAsset> &p_asset, uint32_t p_asset_id);
    static void _release_asset_record(SharedWorld &p_world, uint32_t p_asset_id);
    static bool _is_world_submission_owner_live(ObjectID p_owner_id);
    static void _store_world_submission_record(SharedWorld::WorldSubmissionRecord &r_record, const WorldSubmission &p_submission);
    static void _copy_world_submission_record(const SharedWorld &p_world, const SharedWorld::WorldSubmissionRecord &p_record,
            WorldSubmission *r_submission);
	    static GaussianSplatRenderer::WorldSubmissionContract _build_world_submission_contract(
	            const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot &p_renderer_state,
	            const SharedWorld::WorldSubmissionRecord &p_record);
	    static void _restore_world_submission_renderer(SharedWorld &p_world,
	            const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot &p_snapshot);
	    static bool _apply_world_submission_to_renderer(SharedWorld &p_world, const SharedWorld::WorldSubmissionRecord &p_record,
	            const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot &p_renderer_state);
	bool _should_prune_world(const SharedWorld &p_world) const;
	void _prune_world_if_unused(const RID &p_scenario);
};

#endif // GAUSSIAN_SPLAT_SCENE_DIRECTOR_H

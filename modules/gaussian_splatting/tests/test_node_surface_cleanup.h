#pragma once

// Wave 3 node-surface cleanup pins:
//   - GaussianSplatNode3D no longer exposes ply_file_path / auto_load.
//   - rendering/gaussian_splatting/streaming/route_policy is world-only. The
//     settings accessor still returns the configured value; the node-side
//     invariance contract (direct GaussianSplatNode3D always pins
//     SUBMISSION_RESIDENCY_HINT_RESIDENT) is already pinned in
//     test_scene_director_submission_scaffolding.h.
//   - A strict identity-transform gate (project setting
//     rendering/gaussian_splatting/world/strict_identity_transform) is
//     registered and round-trips through gs::settings::get_bool.
//
#include "test_macros.h"
#include "gs_test_setting_guard.h"

#include "../core/gs_project_settings.h"
#include "../nodes/gaussian_splat_node_3d.h"

#include "core/config/project_settings.h"
#include "core/variant/variant.h"

#if defined(TESTS_ENABLED) || defined(TOOLS_ENABLED)

// ─────────────────────────────────────────────────────────────────────────────
// 1) GaussianSplatNode3D no longer exposes the raw-file compatibility surface.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("[GaussianSplatting][NodeSurface][Cleanup] GaussianSplatNode3D no longer exposes ply_file_path or auto_load") {
	GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
	REQUIRE(node != nullptr);

	bool set_valid = false;
	node->set(StringName("ply_file_path"), String("res://node_surface_cleanup_probe.ply"), &set_valid);
	CHECK_FALSE(set_valid);
	node->set(StringName("auto_load"), true, &set_valid);
	CHECK_FALSE(set_valid);

	bool get_valid = false;
	Variant legacy_path = node->get(StringName("ply_file_path"), &get_valid);
	CHECK_FALSE(get_valid);
	CHECK_EQ(legacy_path.get_type(), Variant::NIL);

	Variant legacy_auto_load = node->get(StringName("auto_load"), &get_valid);
	CHECK_FALSE(get_valid);
	CHECK_EQ(legacy_auto_load.get_type(), Variant::NIL);

	memdelete(node);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2) streaming/route_policy is a real ProjectSettings key and its accessor
//    returns the configured enum. The direct-node invariance contract is
//    pinned in test_scene_director_submission_scaffolding.h's explicit
//    instance submission round-trip; here we only validate the settings
//    surface so the world-only scope statement in gs_project_settings.h does
//    not drift.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("[GaussianSplatting][NodeSurface][RoutePolicy] streaming/route_policy settings accessor round-trips world-only enum") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);
	ProjectSettingGuard guard(ps, "rendering/gaussian_splatting/streaming/route_policy");

	ps->set_setting("rendering/gaussian_splatting/streaming/route_policy",
			(int)gs::settings::GS_ROUTE_RESIDENT);
	CHECK_EQ(gs::settings::get_streaming_route_policy(ps), (int)gs::settings::GS_ROUTE_RESIDENT);

	ps->set_setting("rendering/gaussian_splatting/streaming/route_policy",
			(int)gs::settings::GS_ROUTE_STREAMING);
	CHECK_EQ(gs::settings::get_streaming_route_policy(ps), (int)gs::settings::GS_ROUTE_STREAMING);

	// Token and source strings are part of the diagnostics contract that the
	// world path consumes when publishing its submission hint.
	CHECK_EQ(String(gs::settings::get_streaming_route_policy_token(
						 (int)gs::settings::GS_ROUTE_RESIDENT)),
			String("resident"));
	CHECK_EQ(String(gs::settings::get_streaming_route_policy_token(
						 (int)gs::settings::GS_ROUTE_STREAMING)),
			String("streaming"));
	CHECK_EQ(String(gs::settings::get_streaming_route_policy_source(ps)),
			String("route_policy"));
}

// ─────────────────────────────────────────────────────────────────────────────
// 3) The strict identity-transform gate is registered as a ProjectSettings key
//    with a default of false (warn-only legacy behavior preserved). Setting it
//    true and reading back via gs::settings::get_bool pins the wiring so the
//    GaussianSplatWorld3D::_apply_world_internal hard-fail branch cannot be
//    accidentally disabled by a rename.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("[GaussianSplatting][NodeSurface][World] strict_identity_transform setting round-trips") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);
	ProjectSettingGuard guard(ps, "rendering/gaussian_splatting/world/strict_identity_transform");

	// Default is false (compat). The setting is registered by
	// GaussianSplatManager::initialize_module().
	CHECK_FALSE(gs::settings::get_bool(ps,
			"rendering/gaussian_splatting/world/strict_identity_transform", false));

	ps->set_setting("rendering/gaussian_splatting/world/strict_identity_transform", true);
	CHECK(gs::settings::get_bool(ps,
			"rendering/gaussian_splatting/world/strict_identity_transform", false));

	ps->set_setting("rendering/gaussian_splatting/world/strict_identity_transform", false);
	CHECK_FALSE(gs::settings::get_bool(ps,
			"rendering/gaussian_splatting/world/strict_identity_transform", true));
}

#endif // TESTS_ENABLED || TOOLS_ENABLED

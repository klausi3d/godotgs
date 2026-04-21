#pragma once

// Wave 2 node-surface cleanup pins:
//   - GaussianSplatNode3D::ply_file_path is deprecated. The setter emits
//     WARN_DEPRECATED on non-empty assignment and stays silent when cleared.
//   - rendering/gaussian_splatting/streaming/route_policy is world-only. The
//     settings accessor still returns the configured value; the node-side
//     invariance contract (direct GaussianSplatNode3D always pins
//     SUBMISSION_RESIDENCY_HINT_RESIDENT) is already pinned in
//     test_scene_director_submission_scaffolding.h.
//   - A strict identity-transform gate (project setting
//     rendering/gaussian_splatting/world/strict_identity_transform) is
//     registered and round-trips through gs::settings::get_bool.
//
// Note on warning visibility: WARN_DEPRECATED / WARN_DEPRECATED_MSG latches a
// function-local `static bool warning_shown`, so each deprecation emits at most
// once per process. The shared doctest runner can consume the latch in earlier
// tests, so we never REQUIRE the warning to fire here — we only assert that
// any captured deprecation banner is the one we expect.

#include "test_macros.h"
#include "gs_test_setting_guard.h"

#include "../core/gs_project_settings.h"
#include "../nodes/gaussian_splat_node_3d.h"

#include "core/config/project_settings.h"
#include "core/error/error_macros.h"
#include "core/templates/vector.h"
#include "core/variant/variant.h"

#if defined(TESTS_ENABLED) || defined(TOOLS_ENABLED)

namespace {

// Captures WARN_DEPRECATED / ERR_PRINT traffic routed through the global error
// handler list so tests can inspect deprecation banners without scraping logs.
struct NodeSurfaceMessageCapture : public ErrorHandlerList {
	Vector<String> messages;

	static void _handle(void *p_userdata, const char *, const char *, int,
			const char *p_error, const char *p_message, bool, ErrorHandlerType) {
		NodeSurfaceMessageCapture *self = static_cast<NodeSurfaceMessageCapture *>(p_userdata);
		String combined;
		if (p_error && p_error[0]) {
			combined = String::utf8(p_error);
		}
		if (p_message && p_message[0]) {
			if (!combined.is_empty()) {
				combined += " ";
			}
			combined += String::utf8(p_message);
		}
		if (!combined.is_empty()) {
			self->messages.push_back(combined);
		}
	}

	NodeSurfaceMessageCapture() {
		errfunc = _handle;
		userdata = this;
		add_error_handler(this);
	}

	~NodeSurfaceMessageCapture() {
		remove_error_handler(this);
	}

	bool contains(const String &p_needle) const {
		for (int i = 0; i < messages.size(); i++) {
			if (messages[i].find(p_needle) != -1) {
				return true;
			}
		}
		return false;
	}
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 1) ply_file_path on GaussianSplatNode3D is deprecated. The setter still
//    writes the field (compat preserved for existing scenes); clearing the
//    field stays quiet.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef DISABLE_DEPRECATED
TEST_CASE("[GaussianSplatting][NodeSurface][Deprecation] GaussianSplatNode3D::set_ply_file_path remains a compat-only setter") {
	GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);

	// Functional guard: the deprecated setter still writes the field so that
	// pre-existing scenes load until callers migrate.
	{
		NodeSurfaceMessageCapture capture;
		node->set_ply_file_path("res://node_surface_cleanup_probe.ply");
		CHECK_EQ(node->get_ply_file_path(), String("res://node_surface_cleanup_probe.ply"));
		for (int i = 0; i < capture.messages.size(); i++) {
			if (capture.messages[i].find("deprecated") != -1) {
				CHECK(capture.messages[i].find("ply_file_path") != -1);
			}
		}
	}

	// Clearing the property must not emit a deprecation warning — scripts and
	// tooling commonly null the field out.
	{
		NodeSurfaceMessageCapture capture;
		node->set_ply_file_path(String());
		CHECK_EQ(node->get_ply_file_path(), String());
		CHECK_FALSE(capture.contains("ply_file_path is deprecated"));
	}

	memdelete(node);
}
#endif

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

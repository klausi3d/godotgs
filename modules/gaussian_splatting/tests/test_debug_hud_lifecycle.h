#pragma once

#include "test_macros.h"

#include "../nodes/gaussian_splat_debug_hud.h"
#include "../nodes/gaussian_splat_node_3d.h"

#include "scene/main/scene_tree.h"
#include "scene/main/window.h"

#if defined(TESTS_ENABLED) || defined(TOOLS_ENABLED)

TEST_CASE("[GaussianSplatting][SceneTree] Debug HUD resolves stale splat nodes through ObjectDB") {
	GaussianSplatDebugHUD *hud = memnew(GaussianSplatDebugHUD);
	GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);

	hud->set_splat_node(node);
	CHECK_EQ(hud->get_splat_node(), node);

	memdelete(node);

	CHECK_EQ(hud->get_splat_node(), nullptr);
	hud->refresh_stats();

	memdelete(hud);
}

TEST_CASE("[GaussianSplatting][SceneTree] Debug HUD clears target when splat node exits tree") {
	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree must exist (provided by [SceneTree] tag)");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window must exist");

	GaussianSplatDebugHUD *hud = memnew(GaussianSplatDebugHUD);
	GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);

	root->add_child(node);
	tree->process(0.0);

	hud->set_splat_node(node);
	CHECK_EQ(hud->get_splat_node(), node);

	root->remove_child(node);
	CHECK_EQ(hud->get_splat_node(), nullptr);

	memdelete(node);
	memdelete(hud);
}

TEST_CASE("[GaussianSplatting][SceneTree] Debug HUD preserves owner-controlled processing across re-entry") {
	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree must exist (provided by [SceneTree] tag)");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window must exist");

	GaussianSplatDebugHUD *hud = memnew(GaussianSplatDebugHUD);

	root->add_child(hud);
	tree->process(0.0);
	hud->set_process(false);

	root->remove_child(hud);
	root->add_child(hud);
	tree->process(0.0);

	CHECK_FALSE(hud->is_processing());

	root->remove_child(hud);
	memdelete(hud);
}

TEST_CASE("[GaussianSplatting][SceneTree] GaussianSplatNode3D destroys owned debug HUD on disable and exit") {
	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree must exist (provided by [SceneTree] tag)");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window must exist");

	GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
	root->add_child(node);
	tree->process(0.0);

	node->set_show_performance_hud(true);
	tree->process(0.0);
	CHECK(node->find_child("GaussianSplatDebugHUDLayer", true, false) != nullptr);
	CHECK(node->find_child("GaussianSplatDebugHUD", true, false) != nullptr);

	node->set_show_performance_hud(false);
	tree->process(0.0);
	CHECK(node->find_child("GaussianSplatDebugHUDLayer", true, false) == nullptr);
	CHECK(node->find_child("GaussianSplatDebugHUD", true, false) == nullptr);

	node->set_show_residency_hud(true);
	tree->process(0.0);
	CHECK(node->find_child("GaussianSplatDebugHUDLayer", true, false) != nullptr);
	CHECK(node->find_child("GaussianSplatDebugHUD", true, false) != nullptr);

	root->remove_child(node);
	tree->process(0.0);
	CHECK(node->find_child("GaussianSplatDebugHUDLayer", true, false) == nullptr);
	CHECK(node->find_child("GaussianSplatDebugHUD", true, false) == nullptr);

	memdelete(node);
}

#endif // TESTS_ENABLED || TOOLS_ENABLED

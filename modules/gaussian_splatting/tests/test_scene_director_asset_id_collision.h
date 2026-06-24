#pragma once

// Regression test for the SharedWorld::asset_records aliasing bug.
//
// register_instance() used to derive the asset_records key with
// `static_cast<uint32_t>(p_asset->get_instance_id())`, truncating the 64-bit
// ObjectID to 32 bits. Two assets whose ObjectIDs collide in the low 32 bits
// would then alias the same AssetRecord — one asset's GaussianData would
// silently shadow the other's, and releasing one instance would tear down the
// record still referenced by the other. The fix keys asset_records on the full
// 64-bit ObjectID.

#include "test_macros.h"

#include "../core/gaussian_data.h"
#include "../core/gaussian_splat_asset.h"
#include "../core/gaussian_splat_scene_director.h"
#include "scene/3d/node_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"

#if defined(TESTS_ENABLED) || defined(TOOLS_ENABLED)

namespace {

Ref<GaussianSplatAsset> _make_collision_test_asset(float p_x_offset) {
	Ref<GaussianSplatAsset> asset;
	asset.instantiate();
	asset->set_splat_count(1);

	PackedFloat32Array positions;
	positions.resize(3);
	positions.set(0, p_x_offset);
	positions.set(1, 0.0f);
	positions.set(2, 0.0f);
	asset->set_positions(positions);

	PackedFloat32Array scales;
	scales.resize(3);
	scales.set(0, 1.0f);
	scales.set(1, 1.0f);
	scales.set(2, 1.0f);
	asset->set_scales(scales);

	PackedFloat32Array rotations;
	rotations.resize(4);
	rotations.set(0, 1.0f);
	rotations.set(1, 0.0f);
	rotations.set(2, 0.0f);
	rotations.set(3, 0.0f);
	asset->set_rotations(rotations);

	PackedFloat32Array sh_dc;
	sh_dc.resize(3);
	sh_dc.set(0, 1.0f);
	sh_dc.set(1, 1.0f);
	sh_dc.set(2, 1.0f);
	asset->set_sh_dc_coefficients(sh_dc);

	PackedFloat32Array opacity_logits;
	opacity_logits.resize(1);
	opacity_logits.set(0, 10.0f);
	asset->set_opacity_logits(opacity_logits);

	return asset;
}

} // namespace

// Pure keying invariant: the asset_records key must be injective over a pair of
// ObjectIDs that share the low 32 bits. This is the exact condition the old
// `static_cast<uint32_t>` truncation violated, so it fails on the pre-fix
// keying expression and passes once the full 64-bit ObjectID is used.
TEST_CASE("[GaussianSplatting][SceneDirector] asset_records key does not alias ObjectIDs colliding in the low 32 bits") {
	// Two distinct 64-bit ids that are identical in their low 32 bits.
	const uint64_t low = 0x0000'0000'1234'5678ULL;
	const uint64_t high = 0xABCD'0000'1234'5678ULL;
	REQUIRE(low != high);
	REQUIRE(static_cast<uint32_t>(low) == static_cast<uint32_t>(high)); // the aliasing precondition

	const uint64_t key_low = GaussianSplatSceneDirector::test_asset_records_key(ObjectID(low));
	const uint64_t key_high = GaussianSplatSceneDirector::test_asset_records_key(ObjectID(high));

	// The full-width key must keep them apart; a 32-bit truncating key would not.
	CHECK(key_low != key_high);
	CHECK(key_low == low);
	CHECK(key_high == high);
}

// End-to-end: register two instances backed by two distinct assets in the same
// world and confirm each asset gets its own record keyed on its full ObjectID.
// Runs headless (no RenderingDevice required) because asset_records are built
// before any renderer is attached.
TEST_CASE("[GaussianSplatting][SceneDirector][SceneTree] distinct assets retain distinct asset_records") {
	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree must exist (provided by [SceneTree] tag)");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window must exist");

	Ref<World3D> world = root->get_world_3d();
	REQUIRE(world.is_valid());
	const RID scenario = world->get_scenario();
	REQUIRE(scenario.is_valid());

	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);

	const uint32_t baseline_records = director->test_asset_record_count_for_scenario(scenario);

	Node3D *node_a = memnew(Node3D);
	Node3D *node_b = memnew(Node3D);
	root->add_child(node_a);
	root->add_child(node_b);
	tree->process(0.0);

	Ref<GaussianSplatAsset> asset_a = _make_collision_test_asset(0.0f);
	Ref<GaussianSplatAsset> asset_b = _make_collision_test_asset(10.0f);
	REQUIRE(asset_a.is_valid());
	REQUIRE(asset_b.is_valid());
	// Two distinct assets => two distinct ObjectIDs => two distinct records.
	REQUIRE(asset_a->get_instance_id() != asset_b->get_instance_id());

	director->register_instance(node_a->get_instance_id(), asset_a, Transform3D(), 1.0f, 0.0f, 0u);
	director->register_instance(node_b->get_instance_id(), asset_b, Transform3D(), 1.0f, 0.0f, 0u);

	CHECK_EQ(director->test_asset_record_count_for_scenario(scenario), baseline_records + 2);
	CHECK(director->test_has_asset_record_for_scenario(scenario, asset_a->get_instance_id()));
	CHECK(director->test_has_asset_record_for_scenario(scenario, asset_b->get_instance_id()));

	// Releasing one instance must drop only that asset's record; the other must survive.
	director->unregister_instance(node_a->get_instance_id());
	CHECK_FALSE(director->test_has_asset_record_for_scenario(scenario, asset_a->get_instance_id()));
	CHECK(director->test_has_asset_record_for_scenario(scenario, asset_b->get_instance_id()));

	director->unregister_instance(node_b->get_instance_id());
	CHECK_EQ(director->test_asset_record_count_for_scenario(scenario), baseline_records);

	root->remove_child(node_b);
	root->remove_child(node_a);
	memdelete(node_b);
	memdelete(node_a);
	tree->process(0.0);

	if (owns_director) {
		memdelete(director);
	}
}

#endif // TESTS_ENABLED || TOOLS_ENABLED

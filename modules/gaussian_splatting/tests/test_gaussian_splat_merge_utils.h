#pragma once

#include "test_macros.h"

#include "../core/gaussian_splat_merge_utils.h"
#include "core/math/math_funcs.h"
#include <cstdint>
#include <initializer_list>

namespace {

PackedFloat32Array _merge_test_f32(std::initializer_list<float> p_values) {
	PackedFloat32Array out;
	out.resize((int)p_values.size());
	float *write = out.ptrw();
	int index = 0;
	for (float value : p_values) {
		write[index++] = value;
	}
	return out;
}

PackedInt32Array _merge_test_i32(std::initializer_list<int32_t> p_values) {
	PackedInt32Array out;
	out.resize((int)p_values.size());
	int32_t *write = out.ptrw();
	int index = 0;
	for (int32_t value : p_values) {
		write[index++] = value;
	}
	return out;
}

PackedColorArray _merge_test_colors(std::initializer_list<Color> p_values) {
	PackedColorArray out;
	out.resize((int)p_values.size());
	Color *write = out.ptrw();
	int index = 0;
	for (const Color &value : p_values) {
		write[index++] = value;
	}
	return out;
}

Ref<GaussianSplatAsset> _make_single_splat_merge_asset(
		const Vector3 &p_position,
		const Vector3 &p_scale,
		const Vector3 &p_normal,
		float p_opacity,
		const PackedFloat32Array &p_sh_dc,
		const PackedFloat32Array &p_sh_first_order,
		const Dictionary &p_import_metadata = Dictionary()) {
	Ref<GaussianSplatAsset> asset;
	asset.instantiate();
	asset->set_splat_count(1);
	asset->set_positions(_merge_test_f32({ p_position.x, p_position.y, p_position.z }));
	asset->set_scales(_merge_test_f32({ p_scale.x, p_scale.y, p_scale.z }));
	asset->set_rotations(_merge_test_f32({ 1.0f, 0.0f, 0.0f, 0.0f }));
	asset->set_normals(_merge_test_f32({ p_normal.x, p_normal.y, p_normal.z }));
	asset->set_colors(_merge_test_colors({ Color(0.5f, 0.6f, 0.7f, p_opacity) }));
	asset->set_palette_ids(_merge_test_i32({ 17 }));
	asset->set_brush_override_ids(_merge_test_i32({ 23 }));
	asset->set_brush_axes(_merge_test_f32({ 1.0f, 0.0f }));
	asset->set_stroke_ages(_merge_test_f32({ 0.25f }));
	asset->set_sh_dc_coefficients(p_sh_dc);
	asset->set_sh_first_order_coefficients(p_sh_first_order);
	if (!p_import_metadata.is_empty()) {
		asset->set_import_metadata(p_import_metadata);
	}
	return asset;
}

} // namespace

TEST_CASE("[GaussianSplatting][Merge] Empty source list fails and clears stale output") {
	GaussianSplatMergeResult out;
	out.data.instantiate();
	out.chunks.resize(1);
	out.chunks.write[0].indices.push_back(42);
	out.bounds = AABB(Vector3(1.0f, 2.0f, 3.0f), Vector3(4.0f, 5.0f, 6.0f));

	Vector<GaussianSplatMergeSource> sources;
	CHECK_FALSE(gaussian_splat_merge_sources(sources, 32.0f, out));
	CHECK_FALSE(out.data.is_valid());
	CHECK(out.chunks.is_empty());
	CHECK(out.bounds.position.is_equal_approx(Vector3()));
	CHECK(out.bounds.size.is_equal_approx(Vector3()));
}

TEST_CASE("[GaussianSplatting][Merge] Merge applies transforms and preserves per-splat metadata") {
	Dictionary import_metadata;
	import_metadata[StringName("dc_encoding")] = String("linear_rgb");
	Ref<GaussianSplatAsset> asset = _make_single_splat_merge_asset(
			Vector3(1.0f, 2.0f, 3.0f),
			Vector3(1.0f, 2.0f, 3.0f),
			Vector3(0.0f, 1.0f, 0.0f),
			0.35f,
			_merge_test_f32({ 0.1f, 0.2f, 0.3f }),
			_merge_test_f32({ 0.01f, 0.02f, 0.03f }),
			import_metadata);
	REQUIRE(asset.is_valid());

	GaussianSplatMergeSource source;
	source.asset = asset;
	source.transform = Transform3D(
			Basis(Vector3(-2.0f, 0.0f, 0.0f), Vector3(0.0f, 3.0f, 0.0f), Vector3(0.0f, 0.0f, 4.0f)),
			Vector3(10.0f, 20.0f, 30.0f));
	source.is_2d = true;

	Vector<GaussianSplatMergeSource> sources;
	sources.push_back(source);

	GaussianSplatMergeResult out;
	const bool merged = gaussian_splat_merge_sources(sources, 24.0f, out);
	REQUIRE(merged);
	REQUIRE(out.data.is_valid());
	CHECK(out.data->get_count() == 1);
	CHECK(out.data->get_2d_mode());
	CHECK_FALSE(out.chunks.is_empty());

	const Gaussian merged_gaussian = out.data->get_gaussian(0);
	CHECK(merged_gaussian.position.is_equal_approx(Vector3(8.0f, 26.0f, 42.0f)));
	CHECK(merged_gaussian.scale.is_equal_approx(Vector3(2.0f, 6.0f, 12.0f)));
	CHECK(merged_gaussian.normal.is_equal_approx(Vector3(0.0f, 1.0f, 0.0f)));
	CHECK(Math::is_equal_approx(merged_gaussian.opacity, 0.35f));
	CHECK(gaussian_get_palette_id(merged_gaussian.painterly_meta) == 17);
	CHECK(gaussian_get_brush_override_id(merged_gaussian.painterly_meta) == 23);
	CHECK(gaussian_get_dc_encoding(merged_gaussian.render_meta) == GAUSSIAN_DC_ENCODING_LINEAR_RGB);

	bool found_index_zero = false;
	for (int chunk_idx = 0; chunk_idx < out.chunks.size(); chunk_idx++) {
		const PackedInt32Array &indices = out.chunks[chunk_idx].indices;
		for (int i = 0; i < indices.size(); i++) {
			if (indices[i] == 0) {
				found_index_zero = true;
			}
		}
	}
	CHECK(found_index_zero);
}

TEST_CASE("[GaussianSplatting][Merge] Merge clamps mismatched SH terms and keeps per-source DC encoding") {
	Dictionary linear_metadata;
	linear_metadata[StringName("dc_encoding")] = String("linear_rgb");

	Ref<GaussianSplatAsset> asset_a = _make_single_splat_merge_asset(
			Vector3(0.0f, 0.0f, 0.0f),
			Vector3(1.0f, 1.0f, 1.0f),
			Vector3(0.0f, 1.0f, 0.0f),
			0.9f,
			_merge_test_f32({ 1.0f, 2.0f, 3.0f }),
			_merge_test_f32({ 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f, 17.0f, 18.0f }),
			linear_metadata);
	Ref<GaussianSplatAsset> asset_b = _make_single_splat_merge_asset(
			Vector3(5.0f, 0.0f, 0.0f),
			Vector3(1.0f, 1.0f, 1.0f),
			Vector3(0.0f, 1.0f, 0.0f),
			0.8f,
			_merge_test_f32({ 4.0f, 5.0f, 6.0f }),
			_merge_test_f32({ 20.0f, 21.0f, 22.0f }));
	REQUIRE(asset_a.is_valid());
	REQUIRE(asset_b.is_valid());

	Vector<GaussianSplatMergeSource> sources;
	GaussianSplatMergeSource source_a;
	source_a.asset = asset_a;
	source_a.transform = Transform3D();
	sources.push_back(source_a);

	GaussianSplatMergeSource source_b;
	source_b.asset = asset_b;
	source_b.transform = Transform3D();
	sources.push_back(source_b);

	GaussianSplatMergeResult out;
	const bool merged = gaussian_splat_merge_sources(sources, 8.0f, out);
	REQUIRE(merged);
	REQUIRE(out.data.is_valid());
	CHECK(out.data->get_count() == 2);
	CHECK(out.data->get_sh_first_order_count() == 1);
	CHECK(out.data->get_sh_high_order_count() == 0);

	const PackedFloat32Array sh0 = out.data->get_spherical_harmonics(0);
	const PackedFloat32Array sh1 = out.data->get_spherical_harmonics(1);
	REQUIRE(sh0.size() == 6);
	REQUIRE(sh1.size() == 6);
	CHECK(Math::is_equal_approx(sh0[0], 1.0f));
	CHECK(Math::is_equal_approx(sh0[1], 2.0f));
	CHECK(Math::is_equal_approx(sh0[2], 3.0f));
	CHECK(Math::is_equal_approx(sh0[3], 10.0f));
	CHECK(Math::is_equal_approx(sh0[4], 11.0f));
	CHECK(Math::is_equal_approx(sh0[5], 12.0f));
	CHECK(Math::is_equal_approx(sh1[0], 4.0f));
	CHECK(Math::is_equal_approx(sh1[1], 5.0f));
	CHECK(Math::is_equal_approx(sh1[2], 6.0f));
	CHECK(Math::is_equal_approx(sh1[3], 20.0f));
	CHECK(Math::is_equal_approx(sh1[4], 21.0f));
	CHECK(Math::is_equal_approx(sh1[5], 22.0f));

	const Gaussian merged_a = out.data->get_gaussian(0);
	const Gaussian merged_b = out.data->get_gaussian(1);
	CHECK(gaussian_get_dc_encoding(merged_a.render_meta) == GAUSSIAN_DC_ENCODING_LINEAR_RGB);
	CHECK(gaussian_get_dc_encoding(merged_b.render_meta) == GAUSSIAN_DC_ENCODING_LEGACY_BIAS);

	bool saw_index_zero = false;
	bool saw_index_one = false;
	for (int chunk_idx = 0; chunk_idx < out.chunks.size(); chunk_idx++) {
		const PackedInt32Array &indices = out.chunks[chunk_idx].indices;
		for (int i = 0; i < indices.size(); i++) {
			CHECK(indices[i] >= 0);
			CHECK(indices[i] < 2);
			if (indices[i] == 0) {
				saw_index_zero = true;
			}
			if (indices[i] == 1) {
				saw_index_one = true;
			}
		}
	}
	CHECK(saw_index_zero);
	CHECK(saw_index_one);
}

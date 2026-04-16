#pragma once

#include "../io/gaussian_import_preset.h"
#include "tests/test_macros.h"

namespace TestGaussianSplatting {

TEST_CASE("[GaussianSplatting][Importer] import preset catalog exposes stable baseline contracts") {
	const Vector<GaussianImportPresetDefinition> &presets = gaussian_get_import_presets();
	REQUIRE(presets.size() >= 5);

	CHECK(presets[0].id == String("mobile"));
	CHECK(presets[1].id == String("desktop"));
	CHECK(presets[2].id == String("high"));
	CHECK(presets[3].id == String("ultra"));
	CHECK(presets[4].id == String("development"));

	// IDs are used as import sidecar keys and must remain unique.
	for (int i = 0; i < presets.size(); i++) {
		CHECK_FALSE(presets[i].id.is_empty());
		CHECK(presets[i].default_thumbnail_size > 0);
		for (int j = i + 1; j < presets.size(); j++) {
			CHECK(presets[i].id != presets[j].id);
		}
	}

	const GaussianImportPresetDefinition &ultra = gaussian_get_import_preset_by_name("ultra");
	CHECK(ultra.max_splats == 0);
	CHECK_FALSE(ultra.quantize_positions);
	CHECK_FALSE(ultra.quantize_colors);
	CHECK_FALSE(ultra.quantize_scales);
	CHECK_FALSE(ultra.quantize_rotations);
}

TEST_CASE("[GaussianSplatting][Importer] preset lookup remains case-insensitive and clamps safely") {
	const int desktop_idx = gaussian_find_import_preset_index("desktop");
	REQUIRE(desktop_idx >= 0);

	CHECK(gaussian_find_import_preset_index("DeSkToP") == desktop_idx);
	CHECK(gaussian_find_import_preset_index("nonexistent_preset") == -1);

	const GaussianImportPresetDefinition &first = gaussian_get_import_preset_by_index(-100);
	const GaussianImportPresetDefinition &last = gaussian_get_import_preset_by_index(9999);
	CHECK(first.id == String("mobile"));
	CHECK(last.id == String("development"));
}

TEST_CASE("[GaussianSplatting][Importer] unknown preset names fall back to desktop profile") {
	const GaussianImportPresetDefinition &desktop = gaussian_get_import_preset_by_name("desktop");
	const GaussianImportPresetDefinition &fallback = gaussian_get_import_preset_by_name("totally_unknown");

	CHECK(fallback.id == desktop.id);
	CHECK(fallback.max_splats == desktop.max_splats);
	CHECK(fallback.default_thumbnail_size == desktop.default_thumbnail_size);
}

} // namespace TestGaussianSplatting

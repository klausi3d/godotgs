#pragma once

#include "test_macros.h"

#include "../io/gaussian_data_loader.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"

namespace {

String _gs_loader_fixture_path(const String &p_suffix) {
	const uint64_t ticks = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
	const String base_temp = OS::get_singleton() ? OS::get_singleton()->get_temp_path() : ".";
	return base_temp.path_join("godotgs_data_loader_" + p_suffix + "_" + itos(ticks));
}

void _gs_loader_remove_fixture(const String &p_path) {
	DirAccess::remove_absolute(p_path);
}

Error _gs_loader_write_text_fixture(const String &p_path, const char *p_contents) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return ERR_CANT_CREATE;
	}
	file->store_string(p_contents);
	file.unref();
	return OK;
}

} // namespace

namespace TestGaussianSplatting {

TEST_CASE("[GaussianSplatting][Importer][DataLoader] PLY dispatch captures required-property deficiencies") {
	static const char *k_missing_opacity_ascii_ply = R"(ply
format ascii 1.0
element vertex 1
property float x
property float y
property float z
property float scale_0
property float scale_1
property float scale_2
property float rot_0
property float rot_1
property float rot_2
property float rot_3
property float f_dc_0
property float f_dc_1
property float f_dc_2
end_header
0 0 0 0 0 0 1 0 0 0 0.25 0.5 0.75
)";

	const String ply_path = _gs_loader_fixture_path("missing_opacity.PLY");
	const String cache_path = ply_path.get_basename() + ".gsplatcache";
	const String legacy_cache_path = ply_path.get_basename() + ".gsplatworld";
	REQUIRE_MESSAGE(_gs_loader_write_text_fixture(ply_path, k_missing_opacity_ascii_ply) == OK,
			"Should create ASCII PLY fixture");

	GaussianDataLoadResult result;
	const Error err = load_gaussian_data_from_file(ply_path, result);

	CHECK_MESSAGE(err == OK, "load_gaussian_data_from_file should succeed for valid ASCII PLY fixture");
	CHECK(result.used_ply);
	CHECK_FALSE(result.used_spz);
	CHECK_MESSAGE(result.data.is_valid(), "Data should be populated for valid PLY input");
	if (result.data.is_valid()) {
		CHECK(result.data->get_count() == 1);
	}
	CHECK_MESSAGE(result.missing_required.find("opacity") != -1,
			"Missing required opacity property should be surfaced in the load result");

	_gs_loader_remove_fixture(ply_path);
	_gs_loader_remove_fixture(cache_path);
	_gs_loader_remove_fixture(legacy_cache_path);
}

TEST_CASE("[GaussianSplatting][Importer][DataLoader] Failed SPZ load resets stale result state") {
	GaussianDataLoadResult result;
	result.used_ply = true;
	result.used_spz = true;
	result.missing_required.push_back("stale_required");
	result.missing_optional.push_back("stale_optional");
	result.data.instantiate();
	result.data->resize(1);

	const String missing_spz_path = _gs_loader_fixture_path("missing_asset.spz");
	const Error err = load_gaussian_data_from_file(missing_spz_path, result);

	CHECK_MESSAGE(err != OK, "Loading a missing SPZ file should fail");
	CHECK_FALSE(result.used_ply);
	CHECK_FALSE(result.used_spz);
	CHECK(result.data.is_null());
	CHECK(result.missing_required.is_empty());
	CHECK(result.missing_optional.is_empty());
}

} // namespace TestGaussianSplatting

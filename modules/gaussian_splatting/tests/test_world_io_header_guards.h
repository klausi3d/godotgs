#pragma once

#include "test_macros.h"

#include "../io/gaussian_splat_world_io.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"

namespace {

constexpr uint32_t TEST_WORLD_IO_MAGIC = 0x57505347; // "GSPW".
constexpr uint32_t TEST_WORLD_IO_VERSION = 1;
constexpr uint64_t TEST_WORLD_IO_HEADER_SIZE = 104u;

String _make_world_io_guard_fixture_path(const String &p_prefix) {
	const uint64_t ticks = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
	const String base_temp = OS::get_singleton() ? OS::get_singleton()->get_temp_path() : ".";
	return base_temp.path_join("godotgs_world_io_header_guard_" + p_prefix + "_" + itos(ticks) + ".gsplatworld");
}

void _remove_world_io_guard_fixture(const String &p_path) {
	DirAccess::remove_absolute(p_path);
}

void _store_world_io_header_vec3(const Ref<FileAccess> &p_file, const Vector3 &p_value) {
	p_file->store_float(p_value.x);
	p_file->store_float(p_value.y);
	p_file->store_float(p_value.z);
}

void _write_world_io_header_fixture(const Ref<FileAccess> &p_file,
		uint32_t p_splat_count,
		uint32_t p_sh_degree,
		uint64_t p_gaussian_offset) {
	p_file->store_32(TEST_WORLD_IO_MAGIC);
	p_file->store_32(TEST_WORLD_IO_VERSION);
	p_file->store_32(0u); // flags
	p_file->store_32(p_splat_count);
	p_file->store_32(p_sh_degree);
	p_file->store_32(0u); // sh_first_order
	p_file->store_32(0u); // sh_high_order
	_store_world_io_header_vec3(p_file, Vector3());
	_store_world_io_header_vec3(p_file, Vector3());
	p_file->store_32(0u); // chunk_count
	p_file->store_64(p_gaussian_offset);
	p_file->store_64(TEST_WORLD_IO_HEADER_SIZE); // sh_offset
	p_file->store_64(TEST_WORLD_IO_HEADER_SIZE); // chunk_table_offset
	p_file->store_64(TEST_WORLD_IO_HEADER_SIZE); // indices_offset
	p_file->store_64(0u); // metadata_offset
	p_file->store_64(0u); // metadata_size
}

} // namespace

TEST_CASE("[GaussianSplatting][WorldIO] loader rejects unsupported SH degree from header") {
	const String path = _make_world_io_guard_fixture_path("invalid_sh_degree");

	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	CHECK_MESSAGE(file.is_valid(), "Fixture file should be writable.");
	if (!file.is_valid()) {
		return;
	}

	_write_world_io_header_fixture(file, 0u, 4u, TEST_WORLD_IO_HEADER_SIZE);
	file.unref();

	ResourceFormatLoaderGaussianSplatWorld loader;
	Error load_err = OK;
	Ref<Resource> loaded = loader.load(path, "", &load_err);
	CHECK_MESSAGE(loaded.is_null(), "Loader should reject unsupported SH degree values.");
	CHECK_MESSAGE(load_err == ERR_FILE_CORRUPT, "Unsupported SH degree should return ERR_FILE_CORRUPT.");

	_remove_world_io_guard_fixture(path);
}

TEST_CASE("[GaussianSplatting][WorldIO] loader rejects gaussian payload offset before header") {
	const String path = _make_world_io_guard_fixture_path("gaussian_offset_before_header");

	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	CHECK_MESSAGE(file.is_valid(), "Fixture file should be writable.");
	if (!file.is_valid()) {
		return;
	}

	_write_world_io_header_fixture(file, 0u, 0u, TEST_WORLD_IO_HEADER_SIZE - 4u);
	file.unref();

	ResourceFormatLoaderGaussianSplatWorld loader;
	Error load_err = OK;
	Ref<Resource> loaded = loader.load(path, "", &load_err);
	CHECK_MESSAGE(loaded.is_null(), "Loader should reject gaussian offsets before the fixed header.");
	CHECK_MESSAGE(load_err == ERR_FILE_CORRUPT, "Offsets before the gsplatworld header must be rejected.");

	_remove_world_io_guard_fixture(path);
}

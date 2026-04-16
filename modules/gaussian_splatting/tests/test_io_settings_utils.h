#pragma once

#include "tests/test_macros.h"

#include "gs_test_setting_guard.h"
#include "../io/io_settings_utils.h"

namespace TestIOSettingsUtils {

TEST_CASE("[GaussianSplatting][Importer] get_bool_setting returns fallback when ProjectSettings is null") {
	CHECK_FALSE(GaussianSplattingIO::get_bool_setting(nullptr, StringName("any/path"), false));
	CHECK(GaussianSplattingIO::get_bool_setting(nullptr, StringName("any/path"), true));
}

TEST_CASE("[GaussianSplatting][Importer] get_bool_setting coerces bool, int, and float values") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);

	const String setting_path = "rendering/gaussian_splatting/tests/io_settings_utils/coercion";
	ProjectSettingGuard guard_setting(ps, setting_path);

	ps->set_setting(setting_path, Variant(true));
	CHECK(GaussianSplattingIO::get_bool_setting(ps, setting_path, false));

	ps->set_setting(setting_path, Variant(false));
	CHECK_FALSE(GaussianSplattingIO::get_bool_setting(ps, setting_path, true));

	ps->set_setting(setting_path, Variant(int64_t(3)));
	CHECK(GaussianSplattingIO::get_bool_setting(ps, setting_path, false));

	ps->set_setting(setting_path, Variant(int64_t(0)));
	CHECK_FALSE(GaussianSplattingIO::get_bool_setting(ps, setting_path, true));

	ps->set_setting(setting_path, Variant(2.5));
	CHECK(GaussianSplattingIO::get_bool_setting(ps, setting_path, false));

	ps->set_setting(setting_path, Variant(0.0));
	CHECK_FALSE(GaussianSplattingIO::get_bool_setting(ps, setting_path, true));
}

TEST_CASE("[GaussianSplatting][Importer] get_bool_setting falls back for missing and non-numeric values") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);

	const String missing_path = "rendering/gaussian_splatting/tests/io_settings_utils/missing";
	CHECK_FALSE(GaussianSplattingIO::get_bool_setting(ps, missing_path, false));
	CHECK(GaussianSplattingIO::get_bool_setting(ps, missing_path, true));

	const String non_numeric_path = "rendering/gaussian_splatting/tests/io_settings_utils/non_numeric";
	ProjectSettingGuard guard_setting(ps, non_numeric_path);
	ps->set_setting(non_numeric_path, Variant(String("true")));

	CHECK_FALSE(GaussianSplattingIO::get_bool_setting(ps, non_numeric_path, false));
	CHECK(GaussianSplattingIO::get_bool_setting(ps, non_numeric_path, true));
}

#ifdef GS_SILENCE_LOGS
TEST_CASE("[GaussianSplatting][Importer] is_data_log_enabled is hard-disabled when GS_SILENCE_LOGS is defined") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);

	const String all_debug_path = "rendering/gaussian_splatting/debug/enable_all_debug";
	const String data_log_path = "rendering/gaussian_splatting/debug/enable_data_logging";
	ProjectSettingGuard guard_all_debug(ps, all_debug_path);
	ProjectSettingGuard guard_data_log(ps, data_log_path);

	ps->set_setting(all_debug_path, true);
	ps->set_setting(data_log_path, true);
	CHECK_FALSE(GaussianSplattingIO::is_data_log_enabled());
}
#else
TEST_CASE("[GaussianSplatting][Importer] is_data_log_enabled prefers global debug switch over data logging flag") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);

	const String all_debug_path = "rendering/gaussian_splatting/debug/enable_all_debug";
	const String data_log_path = "rendering/gaussian_splatting/debug/enable_data_logging";
	ProjectSettingGuard guard_all_debug(ps, all_debug_path);
	ProjectSettingGuard guard_data_log(ps, data_log_path);

	ps->set_setting(all_debug_path, true);
	ps->set_setting(data_log_path, false);
	CHECK(GaussianSplattingIO::is_data_log_enabled());
}

TEST_CASE("[GaussianSplatting][Importer] is_data_log_enabled follows data logging flag when global debug is disabled") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);

	const String all_debug_path = "rendering/gaussian_splatting/debug/enable_all_debug";
	const String data_log_path = "rendering/gaussian_splatting/debug/enable_data_logging";
	ProjectSettingGuard guard_all_debug(ps, all_debug_path);
	ProjectSettingGuard guard_data_log(ps, data_log_path);

	ps->set_setting(all_debug_path, false);
	ps->set_setting(data_log_path, true);
	CHECK(GaussianSplattingIO::is_data_log_enabled());

	ps->set_setting(data_log_path, false);
	CHECK_FALSE(GaussianSplattingIO::is_data_log_enabled());
}
#endif

} // namespace TestIOSettingsUtils

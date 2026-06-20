/**************************************************************************/
/*  test_config_validation.h                                              */
/*  Configuration validation unit tests for Gaussian Splatting module     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "tests/test_macros.h"
#include "core/config/project_settings.h"
#include "gs_test_setting_guard.h"
#include "../core/gaussian_splat_manager.h"
#include "../renderer/gpu_sorting_config.h"
#include "../renderer/pipeline_feature_set.h"
#include "../renderer/sorting_config.h"
#include "../renderer/sorting_settings_utils.h"
#include "../renderer/gpu_sorter.h"
#include "../core/gaussian_splat_quality_config.h"
#include "../core/streaming_vram_regulator.h"
#include "../interfaces/gpu_culler.h"
#include "../lod/lod_config.h"

#include <limits>

namespace TestConfigValidation {

static float _reload_manager_sorting_target_ms() {
	GaussianSplatManager *manager = GaussianSplatManager::get_singleton();
	GaussianSplatManager *owned_manager = nullptr;
	if (!manager) {
		owned_manager = memnew(GaussianSplatManager);
		manager = owned_manager;
	}
	ERR_FAIL_NULL_V(manager, 0.0f);
	manager->initialize_module();
	const float target_sort_time_ms = manager->get_sorting_target_ms();
	if (owned_manager) {
		memdelete(owned_manager);
	}
	return target_sort_time_ms;
}

// =============================================================================
// GPUSortingConfig Validation Tests
// =============================================================================

TEST_CASE("[GaussianSplatting][Config] GPUSortingConfig default values pass validation") {
	GPUSortingConfig config;
	config.reset_to_defaults();

	CHECK(config.validate());
	CHECK(config.get_validation_errors().is_empty());
}

TEST_CASE("[GaussianSplatting][Config] Hidden runtime-affecting ProjectSettings are registered with stable defaults") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);

	const StringName renderdoc_key("rendering/gaussian_splatting/renderdoc_compatibility");
	const StringName depth_test_key("rendering/gaussian_splatting/composite/depth_test");
	const StringName effector_frequency_key("rendering/gaussian_splatting/effects/sphere_effector_frequency");
	const StringName vram_budget_key("rendering/gaussian_splatting/streaming/vram_budget_mb");

	CHECK(ps->has_setting(renderdoc_key));
	CHECK(ps->has_setting(depth_test_key));
	CHECK(ps->has_setting(effector_frequency_key));
	CHECK(ps->has_setting(vram_budget_key));

	CHECK_FALSE(bool(ps->get_setting(renderdoc_key)));
	CHECK(bool(ps->get_setting(depth_test_key)));
	CHECK(Math::is_equal_approx(double(ps->get_setting(effector_frequency_key)), 2.0));
	if (ps->property_can_revert(vram_budget_key)) {
		CHECK(int64_t(ps->property_get_revert(vram_budget_key)) == int64_t(STREAMING_UNKNOWN_CAPACITY_FALLBACK_VRAM_BUDGET_MB));
	}
}

TEST_CASE("[GaussianSplatting][Config] GPUSortingConfig rejects invalid target_sort_time_ms") {
	GPUSortingConfig config;
	config.reset_to_defaults();

	SUBCASE("Zero target time is invalid") {
		config.target_sort_time_ms = 0.0f;
		CHECK_FALSE(config.validate());
		CHECK(config.get_validation_errors().contains("Target sort time must be > 0.1ms"));
	}

	SUBCASE("Negative target time is invalid") {
		config.target_sort_time_ms = -1.0f;
		CHECK_FALSE(config.validate());
		CHECK(config.get_validation_errors().contains("Target sort time must be > 0.1ms"));
	}

	SUBCASE("Exactly 0.1ms is invalid (must be greater than)") {
		config.target_sort_time_ms = 0.1f;
		CHECK_FALSE(config.validate());
	}

	SUBCASE("Just above threshold is valid") {
		config.target_sort_time_ms = 0.11f;
		CHECK(config.validate());
	}
}

TEST_CASE("[GaussianSplatting][Config] Sorting target_sort_time_ms follows the canonical project setting") {
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	REQUIRE(project_settings != nullptr);

	const String canonical_path = "rendering/gaussian_splatting/sorting/target_sort_time_ms";
	const String legacy_path = "rendering/gaussian_splatting/gpu_sorting/target_sort_time_ms";
	const String preset_path = GPUSortingConfig::GPU_PRESET_PATH;
	ProjectSettingGuard canonical_guard(project_settings, canonical_path);
	ProjectSettingGuard legacy_guard(project_settings, legacy_path);
	ProjectSettingGuard preset_guard(project_settings, preset_path);
	// Force the manual config path so GPUSortingConfig reads target_sort_time_ms
	// instead of short-circuiting through the default "high" preset.
	project_settings->set_setting(preset_path, "custom");

	SUBCASE("Canonical key wins over the legacy alias for both loaders") {
		project_settings->set_setting(canonical_path, 3.5f);
		project_settings->set_setting(legacy_path, 1.25f);
		initialize_gpu_sorting_config();
		const float manager_target_sort_time_ms = _reload_manager_sorting_target_ms();
		project_settings->emit_signal("settings_changed");

		g_gpu_sorting_config.load_from_project_settings();
		const SortingStrategyConfig strategy_config = SortingStrategyConfig::load_from_project_settings();

		CHECK(g_gpu_sorting_config.target_sort_time_ms == doctest::Approx(3.5f));
		CHECK(strategy_config.target_sort_time_ms == doctest::Approx(3.5f));
		CHECK(manager_target_sort_time_ms == doctest::Approx(3.5f));
	}

	SUBCASE("Explicit canonical default still wins over the legacy alias") {
		project_settings->set_setting(canonical_path, 2.0f);
		project_settings->set_setting(legacy_path, 1.25f);
		initialize_gpu_sorting_config();
		const float manager_target_sort_time_ms = _reload_manager_sorting_target_ms();
		project_settings->emit_signal("settings_changed");

		g_gpu_sorting_config.load_from_project_settings();
		const SortingStrategyConfig strategy_config = SortingStrategyConfig::load_from_project_settings();

		CHECK(g_gpu_sorting_config.target_sort_time_ms == doctest::Approx(2.0f));
		CHECK(strategy_config.target_sort_time_ms == doctest::Approx(2.0f));
		CHECK(manager_target_sort_time_ms == doctest::Approx(2.0f));
	}

	SUBCASE("Runtime canonical edits override legacy alias after startup") {
		project_settings->clear(canonical_path);
		project_settings->set_setting(legacy_path, 1.25f);
		initialize_gpu_sorting_config();
		project_settings->set_setting(canonical_path, 4.0f);
		const float manager_target_sort_time_ms = _reload_manager_sorting_target_ms();
		project_settings->emit_signal("settings_changed");

		g_gpu_sorting_config.load_from_project_settings();
		const SortingStrategyConfig strategy_config = SortingStrategyConfig::load_from_project_settings();

		CHECK(g_gpu_sorting_config.target_sort_time_ms == doctest::Approx(4.0f));
		CHECK(strategy_config.target_sort_time_ms == doctest::Approx(4.0f));
		CHECK(manager_target_sort_time_ms == doctest::Approx(4.0f));
	}

	SUBCASE("Runtime canonical default write overrides the legacy alias immediately") {
		project_settings->clear(canonical_path);
		project_settings->set_setting(legacy_path, 1.25f);
		initialize_gpu_sorting_config();
		project_settings->set_setting(canonical_path, 2.0f);
		const float manager_target_sort_time_ms = _reload_manager_sorting_target_ms();
		project_settings->emit_signal("settings_changed");

		g_gpu_sorting_config.load_from_project_settings();
		const SortingStrategyConfig strategy_config = SortingStrategyConfig::load_from_project_settings();

		CHECK(g_gpu_sorting_config.target_sort_time_ms == doctest::Approx(2.0f));
		CHECK(strategy_config.target_sort_time_ms == doctest::Approx(2.0f));
		CHECK(manager_target_sort_time_ms == doctest::Approx(2.0f));
	}

	SUBCASE("Runtime canonical edits back to the default still override the legacy alias") {
		project_settings->clear(canonical_path);
		project_settings->set_setting(legacy_path, 1.25f);
		initialize_gpu_sorting_config();
		project_settings->set_setting(canonical_path, 4.0f);
		CHECK(_reload_manager_sorting_target_ms() == doctest::Approx(4.0f));
		project_settings->emit_signal("settings_changed");

		g_gpu_sorting_config.load_from_project_settings();
		CHECK(g_gpu_sorting_config.target_sort_time_ms == doctest::Approx(4.0f));
		CHECK(SortingStrategyConfig::load_from_project_settings().target_sort_time_ms == doctest::Approx(4.0f));

		project_settings->set_setting(canonical_path, 2.0f);
		const float manager_target_sort_time_ms = _reload_manager_sorting_target_ms();
		project_settings->emit_signal("settings_changed");

		g_gpu_sorting_config.load_from_project_settings();
		const SortingStrategyConfig strategy_config = SortingStrategyConfig::load_from_project_settings();

		CHECK(g_gpu_sorting_config.target_sort_time_ms == doctest::Approx(2.0f));
		CHECK(strategy_config.target_sort_time_ms == doctest::Approx(2.0f));
		CHECK(manager_target_sort_time_ms == doctest::Approx(2.0f));
	}

	SUBCASE("Legacy alias fallback stays aligned until projects migrate") {
		REQUIRE(project_settings->has_setting(canonical_path));
		project_settings->clear(canonical_path);
		project_settings->set_setting(legacy_path, 1.75f);
		initialize_gpu_sorting_config();
		const float manager_target_sort_time_ms = _reload_manager_sorting_target_ms();
		project_settings->emit_signal("settings_changed");

		g_gpu_sorting_config.load_from_project_settings();
		const SortingStrategyConfig strategy_config = SortingStrategyConfig::load_from_project_settings();

		CHECK(g_gpu_sorting_config.target_sort_time_ms == doctest::Approx(1.75f));
		CHECK(strategy_config.target_sort_time_ms == doctest::Approx(1.75f));
		CHECK(manager_target_sort_time_ms == doctest::Approx(1.75f));
	}
}

TEST_CASE("[GaussianSplatting][Config] Adaptive overlap-budget knobs round-trip through project settings") {
	// Guards the S2 measured-sort-sizing wiring: adaptive_overlap_budget_enabled and
	// max_overlap_records_adaptive_min must be readable from ProjectSettings (both
	// default OFF / 100000), and the adaptive-min accessor must never report a floor
	// above the max_overlap_records hard cap. See gpu_sorting_config.cpp:100-102.
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	REQUIRE(project_settings != nullptr);

	const String preset_path = GPUSortingConfig::GPU_PRESET_PATH;
	const String adaptive_path = GPUSortingConfig::ADAPTIVE_OVERLAP_BUDGET_PATH;
	const String shrink_path = GPUSortingConfig::BOUNDED_BUFFER_SHRINK_PATH;
	const String adaptive_min_path = GPUSortingConfig::MAX_OVERLAP_RECORDS_ADAPTIVE_MIN_PATH;
	const String max_overlap_path = GPUSortingConfig::MAX_OVERLAP_RECORDS_PATH;
	ProjectSettingGuard preset_guard(project_settings, preset_path);
	ProjectSettingGuard adaptive_guard(project_settings, adaptive_path);
	ProjectSettingGuard shrink_guard(project_settings, shrink_path);
	ProjectSettingGuard adaptive_min_guard(project_settings, adaptive_min_path);
	ProjectSettingGuard max_overlap_guard(project_settings, max_overlap_path);

	// Default to the manual ("custom") config path for the load/save subcases; the
	// preset subcases below override this to prove the knobs are ALSO honoured under a
	// named preset (they are orthogonal to the sort layout).
	project_settings->set_setting(preset_path, "custom");

	SUBCASE("Both flags default OFF and adaptive_min defaults to 100000 when unset") {
		project_settings->clear(adaptive_path);
		project_settings->clear(shrink_path);
		project_settings->clear(adaptive_min_path);
		GPUSortingConfig config;
		config.load_from_project_settings();
		CHECK(config.adaptive_overlap_budget_enabled == false);
		CHECK(config.bounded_buffer_shrink_enabled == false);
		CHECK(config.max_overlap_records_adaptive_min == 100000u);
	}

	SUBCASE("Enabling both flags + a custom adaptive_min is reflected in the loaded config") {
		project_settings->set_setting(adaptive_path, true);
		project_settings->set_setting(shrink_path, true);
		project_settings->set_setting(adaptive_min_path, 250000);
		GPUSortingConfig config;
		config.load_from_project_settings();
		CHECK(config.adaptive_overlap_budget_enabled == true);
		CHECK(config.bounded_buffer_shrink_enabled == true);
		CHECK(config.max_overlap_records_adaptive_min == 250000u);
	}

	SUBCASE("adaptive_min accessor is clamped to the max_overlap_records hard cap") {
		project_settings->set_setting(max_overlap_path, 200000);
		project_settings->set_setting(adaptive_min_path, 5000000); // deliberately above the cap
		GPUSortingConfig config;
		config.load_from_project_settings();
		CHECK(config.max_overlap_records == 200000u);
		CHECK(config.max_overlap_records_adaptive_min == 5000000u); // raw stored value is preserved
		CHECK(config.get_overlap_records_adaptive_min() == 200000u); // accessor clamps to the cap
	}

	SUBCASE("Negative/zero adaptive_min is clamped to a safe lower bound, not wrapped huge") {
		// A negative project value must not wrap through uint32_t and (via the accessor's
		// upper clamp) pin the floor at the hard cap — that would silently disable shrink.
		project_settings->set_setting(max_overlap_path, 1000000);
		project_settings->set_setting(adaptive_min_path, -1);
		GPUSortingConfig config;
		config.load_from_project_settings();
		CHECK(config.max_overlap_records_adaptive_min == 1u); // -1 clamped to >=1, NOT 4294967295
		CHECK(config.get_overlap_records_adaptive_min() == 1u);
		CHECK(config.get_overlap_records_adaptive_min() < config.max_overlap_records);

		// Zero is clamped to the same >=1 floor (the title's other half).
		project_settings->set_setting(adaptive_min_path, 0);
		config.load_from_project_settings();
		CHECK(config.max_overlap_records_adaptive_min == 1u);
	}

	SUBCASE("GLOBAL_DEF registers the new keys with their defaults") {
		// Round-trips the GLOBAL_DEF wiring (not just save_to_project_settings, which
		// creates the keys itself): clear the keys, re-run registration, and confirm
		// they reappear with the documented defaults.
		project_settings->clear(adaptive_path);
		project_settings->clear(adaptive_min_path);
		initialize_gpu_sorting_config();
		CHECK(project_settings->has_setting(adaptive_path));
		CHECK(project_settings->has_setting(adaptive_min_path));
		CHECK(bool(project_settings->get_setting(adaptive_path)) == false);
		CHECK(int64_t(project_settings->get_setting(adaptive_min_path)) == 100000);
	}

	SUBCASE("reset_to_defaults restores both flags OFF and adaptive_min to 100000") {
		GPUSortingConfig config;
		config.adaptive_overlap_budget_enabled = true;
		config.bounded_buffer_shrink_enabled = true;
		config.max_overlap_records_adaptive_min = 777000;
		config.reset_to_defaults();
		CHECK(config.adaptive_overlap_budget_enabled == false);
		CHECK(config.bounded_buffer_shrink_enabled == false);
		CHECK(config.max_overlap_records_adaptive_min == 100000u);
	}

	SUBCASE("Flags are honored under a named preset, not only custom") {
		// The reclaim knobs are orthogonal to the sort layout, so they must be
		// reachable on the default "high" preset — otherwise the VRAM wins are
		// unavailable to anyone who has not switched to gpu_preset="custom".
		project_settings->set_setting(preset_path, "high");
		project_settings->set_setting(adaptive_path, true);
		project_settings->set_setting(shrink_path, true);
		project_settings->set_setting(adaptive_min_path, 300000);
		GPUSortingConfig config;
		config.load_from_project_settings();
		CHECK(config.adaptive_overlap_budget_enabled == true);
		CHECK(config.bounded_buffer_shrink_enabled == true);
		CHECK(config.max_overlap_records_adaptive_min == 300000u);
	}

	SUBCASE("apply_preset establishes the off baseline, clearing stale custom state") {
		GPUSortingConfig config;
		config.adaptive_overlap_budget_enabled = true;
		config.bounded_buffer_shrink_enabled = true;
		config.max_overlap_records_adaptive_min = 999000;
		REQUIRE(config.apply_preset("high"));
		CHECK(config.adaptive_overlap_budget_enabled == false);
		CHECK(config.bounded_buffer_shrink_enabled == false);
		CHECK(config.max_overlap_records_adaptive_min == 100000u);
	}
}

TEST_CASE("[GaussianSplatting][Config] GPUSortingConfig rejects invalid max_sort_elements") {
	GPUSortingConfig config;
	config.reset_to_defaults();

	SUBCASE("Zero elements is invalid") {
		config.max_sort_elements = 0;
		CHECK_FALSE(config.validate());
		CHECK(config.get_validation_errors().contains("Max sort elements must be > 1000"));
	}

	SUBCASE("1000 elements is invalid (must be greater than)") {
		config.max_sort_elements = 1000;
		CHECK_FALSE(config.validate());
	}

	SUBCASE("1001 elements is valid") {
		config.max_sort_elements = 1001;
		CHECK(config.validate());
	}

	SUBCASE("Large element count is valid") {
		config.max_sort_elements = 100000000;
		CHECK(config.validate());
	}
}

TEST_CASE("[GaussianSplatting][Config] GPUSortingConfig rejects invalid radix_bits") {
	GPUSortingConfig config;
	config.reset_to_defaults();

	SUBCASE("4-bit radix is valid") {
		config.radix_bits = 4;
		CHECK(config.validate());
	}

	SUBCASE("8-bit radix is valid") {
		config.radix_bits = 8;
		CHECK(config.validate());
	}

	SUBCASE("Other radix values are invalid") {
		uint32_t invalid_values[] = {0, 1, 2, 3, 5, 6, 7, 9, 16, 32};
		for (uint32_t value : invalid_values) {
			config.radix_bits = value;
			CHECK_FALSE(config.validate());
			CHECK(config.get_validation_errors().contains("Radix bits must be 4 or 8"));
		}
	}
}

TEST_CASE("[GaussianSplatting][Config] GPUSortingConfig rejects invalid workgroup_size") {
	GPUSortingConfig config;
	config.reset_to_defaults();

	SUBCASE("Valid workgroup sizes") {
		uint32_t valid_sizes[] = {64, 128, 256, 512};
		for (uint32_t size : valid_sizes) {
			config.workgroup_size = size;
			CHECK(config.validate());
		}
	}

	SUBCASE("Invalid workgroup sizes") {
		uint32_t invalid_sizes[] = {0, 1, 32, 63, 65, 100, 255, 257, 1024};
		for (uint32_t size : invalid_sizes) {
			config.workgroup_size = size;
			CHECK_FALSE(config.validate());
			CHECK(config.get_validation_errors().contains("Workgroup size must be 64, 128, 256, or 512"));
		}
	}
}

TEST_CASE("[GaussianSplatting][Config] GPUSortingConfig rejects invalid key_bits") {
	GPUSortingConfig config;
	config.reset_to_defaults();

	SUBCASE("32-bit keys are valid") {
		config.key_bits = 32;
		config.tile_bits = 16;
		config.depth_bits = 16;
		CHECK(config.validate());
	}

	SUBCASE("64-bit keys are valid") {
		config.key_bits = 64;
		CHECK(config.validate());
	}

	SUBCASE("Other key widths are invalid") {
		uint32_t invalid_widths[] = {0, 8, 16, 24, 48, 128};
		for (uint32_t width : invalid_widths) {
			config.key_bits = width;
			// Ensure tile+depth fits for valid test
			config.tile_bits = 1;
			config.depth_bits = 1;
			CHECK_FALSE(config.validate());
			CHECK(config.get_validation_errors().contains("Key bits must be 32 or 64"));
		}
	}
}

TEST_CASE("[GaussianSplatting][Config] Project settings apply preset layouts unless preset is custom") {
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	REQUIRE(project_settings != nullptr);

	const GPUSortingConfig previous_global_config = g_gpu_sorting_config;
	const String preset_path = GPUSortingConfig::GPU_PRESET_PATH;
	const String key_bits_path = GPUSortingConfig::KEY_BITS_PATH;
	const String tile_bits_path = GPUSortingConfig::TILE_BITS_PATH;
	const String depth_bits_path = GPUSortingConfig::DEPTH_BITS_PATH;
	const String tie_breaker_path = GPUSortingConfig::ENABLE_TIE_BREAKER_PATH;
	const String stale_use_32bit_keys_path = GPUSortingConfig::SECTION_PATH + "use_32bit_keys";
	ProjectSettingGuard preset_guard(project_settings, preset_path);
	ProjectSettingGuard key_bits_guard(project_settings, key_bits_path);
	ProjectSettingGuard tile_bits_guard(project_settings, tile_bits_path);
	ProjectSettingGuard depth_bits_guard(project_settings, depth_bits_path);
	ProjectSettingGuard tie_breaker_guard(project_settings, tie_breaker_path);
	ProjectSettingGuard stale_use_32bit_keys_guard(project_settings, stale_use_32bit_keys_path);

	auto apply_explicit_32bit_layout = [&]() {
		project_settings->set_setting(key_bits_path, 32);
		project_settings->set_setting(tile_bits_path, 16);
		project_settings->set_setting(depth_bits_path, 16);
		project_settings->set_setting(tie_breaker_path, true);
	};

	auto check_loaded_32bit_layout = [&]() {
		g_gpu_sorting_config.load_from_project_settings();
		CHECK(g_gpu_sorting_config.key_bits == 32);
		CHECK(g_gpu_sorting_config.tile_bits == 16);
		CHECK(g_gpu_sorting_config.depth_bits == 16);
		CHECK(g_gpu_sorting_config.enable_tie_breaker);

		const SortKeyConfig sort_key_config = SortKeyConfig::from_settings();
		CHECK(sort_key_config.key_bits == 32);
		CHECK(sort_key_config.tile_bits == 16);
		CHECK(sort_key_config.depth_bits == 16);
		CHECK(sort_key_config.enable_tie_breaker);
	};

	SUBCASE("Named presets keep their own key layout") {
		struct PresetExpectation {
			const char *name;
			uint32_t key_bits;
			uint32_t tile_bits;
			uint32_t depth_bits;
			bool tie_breaker;
		};
		const PresetExpectation preset_expectations[] = {
			{ "low", 32, 16, 16, false },
			{ "medium", 64, 32, 32, false },
			{ "high", 64, 32, 32, false },
			{ "ultra", 64, 32, 32, true },
		};
		for (const PresetExpectation &preset : preset_expectations) {
			project_settings->set_setting(preset_path, preset.name);
			apply_explicit_32bit_layout();

			g_gpu_sorting_config.load_from_project_settings();
			CHECK(g_gpu_sorting_config.key_bits == preset.key_bits);
			CHECK(g_gpu_sorting_config.tile_bits == preset.tile_bits);
			CHECK(g_gpu_sorting_config.depth_bits == preset.depth_bits);
			CHECK(g_gpu_sorting_config.enable_tie_breaker == preset.tie_breaker);

			const SortKeyConfig sort_key_config = SortKeyConfig::from_settings();
			CHECK(sort_key_config.key_bits == preset.key_bits);
			CHECK(sort_key_config.tile_bits == preset.tile_bits);
			CHECK(sort_key_config.depth_bits == preset.depth_bits);
			CHECK(sort_key_config.enable_tie_breaker == preset.tie_breaker);
		}

		project_settings->set_setting(preset_path, "ultra");
		g_gpu_sorting_config.load_from_project_settings();
		CHECK(g_gpu_sorting_config.key_bits == 64);
		CHECK(g_gpu_sorting_config.tile_bits == 32);
		CHECK(g_gpu_sorting_config.depth_bits == 32);
		CHECK(g_gpu_sorting_config.enable_tie_breaker);
	}

	SUBCASE("Custom loading honors explicit key-layout settings") {
		project_settings->set_setting(preset_path, "custom");
		apply_explicit_32bit_layout();
		check_loaded_32bit_layout();
	}

	SUBCASE("Stale boolean key-width setting is ignored in favor of canonical key_bits") {
		project_settings->set_setting(preset_path, "custom");

		project_settings->set_setting(key_bits_path, 64);
		project_settings->set_setting(tile_bits_path, 32);
		project_settings->set_setting(depth_bits_path, 32);
		project_settings->set_setting(tie_breaker_path, false);
		project_settings->set_setting(stale_use_32bit_keys_path, true);
		g_gpu_sorting_config.load_from_project_settings();
		CHECK(g_gpu_sorting_config.key_bits == 64);
		CHECK(g_gpu_sorting_config.tile_bits == 32);
		CHECK(g_gpu_sorting_config.depth_bits == 32);
		CHECK_FALSE(g_gpu_sorting_config.enable_tie_breaker);
		CHECK(SortKeyConfig::from_settings().key_bits == 64);

		project_settings->set_setting(key_bits_path, 32);
		project_settings->set_setting(tile_bits_path, 16);
		project_settings->set_setting(depth_bits_path, 16);
		project_settings->set_setting(tie_breaker_path, true);
		project_settings->set_setting(stale_use_32bit_keys_path, false);
		check_loaded_32bit_layout();
	}

	g_gpu_sorting_config = previous_global_config;
}

TEST_CASE("[GaussianSplatting][Config] GPUSortingConfig validates tile/depth bit allocation") {
	GPUSortingConfig config;
	config.reset_to_defaults();

	SUBCASE("Tile and depth bits must not exceed key_bits") {
		config.key_bits = 32;
		config.tile_bits = 20;
		config.depth_bits = 20; // 40 > 32
		CHECK_FALSE(config.validate());
		CHECK(config.get_validation_errors().contains("Tile/depth bit split must fit within key_bits"));
	}

	SUBCASE("Tile and depth bits must allocate at least one bit") {
		config.tile_bits = 0;
		config.depth_bits = 0;
		CHECK_FALSE(config.validate());
		CHECK(config.get_validation_errors().contains("Tile/depth bit split must allocate at least one bit"));
	}

	SUBCASE("Valid 32-bit allocation") {
		config.key_bits = 32;
		config.tile_bits = 16;
		config.depth_bits = 16;
		CHECK(config.validate());
	}

	SUBCASE("Valid 64-bit allocation") {
		config.key_bits = 64;
		config.tile_bits = 32;
		config.depth_bits = 32;
		CHECK(config.validate());
	}

	SUBCASE("Partial allocation is valid") {
		config.key_bits = 64;
		config.tile_bits = 24;
		config.depth_bits = 24; // Only 48 bits used of 64
		CHECK(config.validate());
	}
}

TEST_CASE("[GaussianSplatting][Config] GPUSortingConfig rejects invalid performance_log_interval") {
	GPUSortingConfig config;
	config.reset_to_defaults();

	SUBCASE("Zero interval is invalid") {
		config.performance_log_interval = 0;
		CHECK_FALSE(config.validate());
		CHECK(config.get_validation_errors().contains("Performance log interval must be > 0"));
	}

	SUBCASE("Positive interval is valid") {
		config.performance_log_interval = 1;
		CHECK(config.validate());

		config.performance_log_interval = 1000;
		CHECK(config.validate());
	}
}

TEST_CASE("[GaussianSplatting][Config] GPUSortingConfig accumulates multiple errors") {
	GPUSortingConfig config;
	config.reset_to_defaults();

	// Set multiple invalid values
	config.target_sort_time_ms = 0.0f;
	config.max_sort_elements = 0;
	config.radix_bits = 3;
	config.workgroup_size = 100;
	config.key_bits = 16;
	config.performance_log_interval = 0;

	CHECK_FALSE(config.validate());

	String errors = config.get_validation_errors();
	CHECK(errors.contains("Target sort time must be > 0.1ms"));
	CHECK(errors.contains("Max sort elements must be > 1000"));
	CHECK(errors.contains("Radix bits must be 4 or 8"));
	CHECK(errors.contains("Workgroup size must be 64, 128, 256, or 512"));
	CHECK(errors.contains("Key bits must be 32 or 64"));
	CHECK(errors.contains("Performance log interval must be > 0"));
}

TEST_CASE("[GaussianSplatting][Config] PipelineFeatureSet default values pass validation") {
	PipelineFeatureSet config;
	config.reset_to_defaults();

	CHECK(config.validate());
	CHECK(config.get_validation_errors().is_empty());
}

TEST_CASE("[GaussianSplatting][Config] PipelineFeatureSet validates SH amortization settings only when active") {
	PipelineFeatureSet config;
	config.reset_to_defaults();

	SUBCASE("Inactive SH amortization tolerates stale divisor values") {
		config.sh_amortization_divisor = 0;
		CHECK(config.validate());
	}

	SUBCASE("Divisor must be greater than one when the feature is active") {
		config.enable_sh_amortization = true;
		config.sh_amortization_divisor = 1;
		CHECK_FALSE(config.validate());
		CHECK(config.get_validation_errors().contains("SH amortization divisor must be > 1."));
	}

	SUBCASE("Visibility threshold must be finite") {
		config.enable_sh_amortization = true;
		config.sh_amortization_visibility_threshold = std::numeric_limits<float>::infinity();
		CHECK_FALSE(config.validate());
		CHECK(config.get_validation_errors().contains("SH amortization visibility threshold must be finite."));
	}

	SUBCASE("Visibility threshold must stay within normalized range") {
		config.enable_sh_amortization = true;
		config.sh_amortization_visibility_threshold = 1.5f;
		CHECK_FALSE(config.validate());
		CHECK(config.get_validation_errors().contains("SH amortization visibility threshold must be <= 1."));
	}

	SUBCASE("Disabled visibility invalidation ignores the threshold value") {
		config.enable_sh_amortization = true;
		config.disable_sh_amortization_on_visibility_change = false;
		config.sh_amortization_divisor = 2;
		config.sh_amortization_visibility_threshold = 1.5f;
		CHECK(config.validate());
	}

	SUBCASE("Normalized threshold is accepted") {
		config.enable_sh_amortization = true;
		config.sh_amortization_divisor = 4;
		config.sh_amortization_visibility_threshold = 0.5f;
		CHECK(config.validate());
	}

	SUBCASE("Experimental bundle inherits SH amortization validation") {
		config.enable_all_experimental = true;
		config.sh_amortization_divisor = 1;
		CHECK_FALSE(config.validate());
		CHECK(config.get_validation_errors().contains("SH amortization divisor must be > 1."));
	}
}

TEST_CASE("[GaussianSplatting][Config] PipelineFeatureSet validates packed stage limits when scene size is known") {
	PipelineFeatureSet config;
	config.reset_to_defaults();

	SUBCASE("Packed stage accepts unknown scene size") {
		config.enable_packed_stage_data = true;
		CHECK(config.validate());
	}

	SUBCASE("Packed stage accepts scenes within the 16-bit index budget") {
		config.enable_packed_stage_data = true;
		CHECK(config.validate(PipelineFeatureSet::PACKED_STAGE_MAX_TOTAL_SPLATS));
	}

	SUBCASE("Packed stage rejects oversized scenes") {
		config.enable_packed_stage_data = true;
		CHECK_FALSE(config.validate(PipelineFeatureSet::PACKED_STAGE_MAX_TOTAL_SPLATS + 1));
		CHECK(config.get_validation_errors(PipelineFeatureSet::PACKED_STAGE_MAX_TOTAL_SPLATS + 1)
				.contains("Packed stage data requires <="));
	}

	SUBCASE("Experimental bundle inherits packed stage limits") {
		config.enable_all_experimental = true;
		CHECK_FALSE(config.validate(PipelineFeatureSet::PACKED_STAGE_MAX_TOTAL_SPLATS + 1));
		CHECK(config.get_validation_errors(PipelineFeatureSet::PACKED_STAGE_MAX_TOTAL_SPLATS + 1)
				.contains("Packed stage data requires <="));
	}
}

// =============================================================================
// SortingStrategyConfig Sanitize Tests
// =============================================================================

TEST_CASE("[GaussianSplatting][Config] SortingStrategyConfig sanitize corrects invalid values") {
	SortingStrategyConfig config;

	SUBCASE("Zero bitonic_max_elements corrected to 1") {
		config.bitonic_max_elements = 0;
		config.sanitize();
		CHECK(config.bitonic_max_elements == 1);
	}

	SUBCASE("radix_max_elements enforced >= bitonic_max_elements") {
		config.bitonic_max_elements = 10000;
		config.radix_max_elements = 5000; // Less than bitonic
		config.sanitize();
		CHECK(config.radix_max_elements >= config.bitonic_max_elements);
	}

	SUBCASE("onesweep_max_elements enforced >= radix_max_elements") {
		config.radix_max_elements = 100000;
		config.onesweep_max_elements = 50000; // Less than radix
		config.sanitize();
		CHECK(config.onesweep_max_elements >= config.radix_max_elements);
	}

	SUBCASE("hybrid_trigger_elements enforced >= radix_max_elements") {
		config.radix_max_elements = 100000;
		config.hybrid_trigger_elements = 50000;
		config.sanitize();
		CHECK(config.hybrid_trigger_elements >= config.radix_max_elements);
	}

	SUBCASE("Zero hybrid_batch_size defaults to radix_max_elements") {
		config.radix_max_elements = 100000;
		config.hybrid_batch_size = 0;
		config.sanitize();
		CHECK(config.hybrid_batch_size == config.radix_max_elements);
	}

	SUBCASE("Zero history_size defaults to 120") {
		config.history_size = 0;
		config.sanitize();
		CHECK(config.history_size == 120);
	}

	SUBCASE("Zero log_interval_frames defaults to 60") {
		config.log_interval_frames = 0;
		config.sanitize();
		CHECK(config.log_interval_frames == 60);
	}

	SUBCASE("Negative target_sort_time_ms clamped to 0") {
		config.target_sort_time_ms = -5.0f;
		config.sanitize();
		CHECK(config.target_sort_time_ms == 0.0f);
	}
}

TEST_CASE("[GaussianSplatting][Config] SortingStrategyConfig describe_thresholds format") {
	SortingStrategyConfig config;
	config.bitonic_max_elements = 131072;
	config.radix_max_elements = 1500000;
	config.onesweep_max_elements = 3000000;
	config.hybrid_trigger_elements = 3000000;
	config.hybrid_batch_size = 1500000;

	String description = config.describe_thresholds();

	CHECK(description.contains("131072"));
	CHECK(description.contains("1500000"));
	CHECK(description.contains("3000000"));
}

// =============================================================================
// SortKeyConfig Tests
// =============================================================================

TEST_CASE("[GaussianSplatting][Config] SortKeyConfig default values") {
	SortKeyConfig config;

	CHECK(config.key_bits == 64);
	CHECK(config.tile_bits == 32);
	CHECK(config.depth_bits == 32);
	CHECK(config.enable_tie_breaker == false);
}

TEST_CASE("[GaussianSplatting][Config] SortKeyConfig bit allocation consistency") {
	SortKeyConfig config;

	SUBCASE("Default allocation fits in key") {
		CHECK(config.tile_bits + config.depth_bits <= config.key_bits);
	}

	SUBCASE("32-bit key allocation") {
		config.key_bits = 32;
		config.tile_bits = 16;
		config.depth_bits = 16;
		CHECK(config.tile_bits + config.depth_bits == config.key_bits);
	}
}

// =============================================================================
// Live LODConfig validation
// =============================================================================

TEST_CASE("[GaussianSplatting][Config] LODConfig calculate_lod_level handles disabled and near-zero distances") {
	LODConfig config;
	config.reset_to_defaults();
	config.enabled = false;

	CHECK(config.calculate_lod_level(0.0f) == 0);
	CHECK(config.calculate_lod_level(25.0f) == 0);
	CHECK(config.calculate_lod_level(100.0f) == 0);

	config.enabled = true;

	CHECK(config.calculate_lod_level(0.0f) == 0);
	CHECK(config.calculate_lod_level(0.0001f) == 0);
	CHECK(config.calculate_lod_level(0.001f) == 0);
	CHECK(config.calculate_lod_level(12.5f) == 0);
}

TEST_CASE("[GaussianSplatting][Config] LODConfig calculate_lod_level boundary mapping is explicit") {
	LODConfig config;
	config.reset_to_defaults();
	config.enabled = true;
	config.num_levels = 4;
	config.max_distance = 100.0f;
	config.base_threshold = 10.0f;

	CHECK(config.calculate_lod_level(24.9999f) == 0);
	CHECK_MESSAGE(config.calculate_lod_level(25.0f) == 1,
			"Exact 25.0 enters LOD 1 because calculate_lod_level() is driven by max_distance/num_levels.");
	CHECK(config.calculate_lod_level(25.0001f) == 1);
	CHECK(config.calculate_lod_level(49.9999f) == 1);
	CHECK_MESSAGE(config.calculate_lod_level(50.0f) == 2,
			"Exact 50.0 enters LOD 2 under the current live floor/clamp mapping.");
	CHECK(config.calculate_lod_level(50.0001f) == 2);
	CHECK(config.calculate_lod_level(99.9999f) == 2);
	CHECK_MESSAGE(config.calculate_lod_level(100.0f) == 3,
			"Exact max_distance lands on the farthest LOD level in the live implementation.");
	CHECK(config.calculate_lod_level(100.0001f) == 3);
	CHECK(config.calculate_lod_level(1000.0f) == 3);
}

TEST_CASE("[GaussianSplatting][Config] LODConfig distance thresholds follow base-threshold progression") {
	LODConfig config;
	config.reset_to_defaults();
	config.base_threshold = 10.0f;
	config.max_distance = 100.0f;

	CHECK(config.get_distance_threshold(-1) == doctest::Approx(10.0f));
	CHECK(config.get_distance_threshold(0) == doctest::Approx(10.0f));
	CHECK(config.get_distance_threshold(1) == doctest::Approx(20.0f));
	CHECK(config.get_distance_threshold(2) == doctest::Approx(40.0f));
	CHECK(config.get_distance_threshold(3) == doctest::Approx(80.0f));
	CHECK_MESSAGE(config.get_distance_threshold(4) == doctest::Approx(100.0f),
			"Distance thresholds double from base_threshold and clamp at max_distance.");
}

TEST_CASE("[GaussianSplatting][Config] LODConfig helper mappings match the live implementation") {
	LODConfig config;
	config.reset_to_defaults();
	config.base_threshold = 10.0f;
	config.max_distance = 100.0f;

	CHECK(config.get_splat_skip_factor(0) == 1);
	CHECK(config.get_splat_skip_factor(1) == 2);
	CHECK(config.get_splat_skip_factor(2) == 4);
	CHECK(config.get_splat_skip_factor(3) == 8);

	config.splat_skip_enabled = false;
	CHECK(config.get_splat_skip_factor(3) == 1);
	config.splat_skip_enabled = true;

	CHECK(config.get_sh_band_for_lod(0) == 3);
	CHECK(config.get_sh_band_for_lod(1) == 2);
	CHECK(config.get_sh_band_for_lod(2) == 1);
	CHECK(config.get_sh_band_for_lod(3) == 0);
	CHECK(config.get_sh_band_for_lod(4) == 0);

	config.sh_reduction_enabled = false;
	CHECK(config.get_sh_band_for_lod(3) == 3);
	config.sh_reduction_enabled = true;

	CHECK(config.get_opacity_multiplier(0.0f) == doctest::Approx(1.0f));
	CHECK(config.get_opacity_multiplier(10.0f) == doctest::Approx(1.0f));
	CHECK(config.get_opacity_multiplier(55.0f) == doctest::Approx(0.5f));
	CHECK(config.get_opacity_multiplier(100.0f) == doctest::Approx(0.0f));
	CHECK(config.get_opacity_multiplier(120.0f) == doctest::Approx(0.0f));

	config.opacity_fade_enabled = false;
	CHECK(config.get_opacity_multiplier(55.0f) == doctest::Approx(1.0f));
}

// =============================================================================
// Node-facing LOD/Streaming config validation
// =============================================================================

TEST_CASE("[GaussianSplatting][Config] GaussianSplatLODConfig defaults match live node expectations") {
	using namespace GaussianSplatting;
	GaussianSplatLODConfig config;

	CHECK(config.lod0_distance < config.lod1_distance);
	CHECK(config.lod1_distance < config.lod2_distance);
	CHECK(config.lod2_distance < config.lod3_distance);
	CHECK(config.lod3_distance < config.cull_distance);
	CHECK(config.min_splats_per_frame < config.max_splats_per_frame);
	CHECK(config.importance_threshold >= 0.0f);
	CHECK(config.importance_threshold <= 1.0f);
	CHECK(config.size_cull_threshold > 0.0f);
	CHECK(config.lod_bias > 0.0f);
}

TEST_CASE("[GaussianSplatting][Config] GaussianSplatStreamingConfig defaults match live node expectations") {
	using namespace GaussianSplatting;
	GaussianSplatStreamingConfig config;

	CHECK(config.max_gpu_memory > 0);
	CHECK(config.target_gpu_memory > 0);
	CHECK(config.target_gpu_memory <= config.max_gpu_memory);
	CHECK(config.max_cpu_memory >= config.max_gpu_memory);
	CHECK(config.load_ahead_distance > 0.0f);
	CHECK(config.unload_distance > config.load_ahead_distance);
	CHECK(config.max_concurrent_loads > 0);
	CHECK(config.num_lod_levels >= 2);
	CHECK(config.stream_budget_ms > 0);
}

// =============================================================================
// CullingConfig Validation Tests (GPUCuller::CullingConfig)
// =============================================================================

TEST_CASE("[GaussianSplatting][Config] CullingConfig default values are sensible") {
	GPUCuller::CullingConfig config;

	// Boolean defaults
	CHECK(config.lod_enabled == true);
	CHECK(config.frustum_culling == true);
	CHECK(config.gpu_culling_enabled == true);
	CHECK(config.temporal_coherence == true);

	// Numeric defaults should be positive where expected
	CHECK(config.lod_bias > 0.0f);
	CHECK(config.lod_min_screen_size > 0.0f);
	CHECK(config.lod_max_distance > 0.0f);
	CHECK(config.cull_radius_multiplier > 0.0f);
	CHECK(config.cull_frustum_plane_slack >= 0.0f);
}

TEST_CASE("[GaussianSplatting][Config] CullingConfig tolerance values") {
	GPUCuller::CullingConfig config;

	// Tolerances should be small positive values
	CHECK(config.cull_near_tolerance >= 0.0f);
	CHECK(config.cull_near_tolerance <= 1.0f);
	CHECK(config.cull_far_tolerance >= 0.0f);
	CHECK(config.cull_far_tolerance <= 1.0f);
}

TEST_CASE("[GaussianSplatting][Config] CullingConfig viewport size") {
	GPUCuller::CullingConfig config;

	// Default viewport size should be valid
	CHECK(config.last_cull_viewport_size.x > 0);
	CHECK(config.last_cull_viewport_size.y > 0);
}

// =============================================================================
// Edge Case Tests
// =============================================================================

TEST_CASE("[GaussianSplatting][Config] GPUSortingConfig edge case: maximum valid values") {
	GPUSortingConfig config;
	config.reset_to_defaults();

	// Test maximum reasonable values
	config.target_sort_time_ms = 1000.0f; // 1 second
	config.max_sort_elements = UINT32_MAX;
	config.performance_log_interval = UINT32_MAX;

	CHECK(config.validate());
}

TEST_CASE("[GaussianSplatting][Config] GPUSortingConfig edge case: minimum valid values") {
	GPUSortingConfig config;
	config.reset_to_defaults();

	// Test minimum valid values
	config.target_sort_time_ms = 0.11f; // Just above 0.1
	config.max_sort_elements = 1001; // Just above 1000
	config.radix_bits = 4;
	config.workgroup_size = 64; // Smallest valid
	config.key_bits = 32; // Smallest valid
	config.tile_bits = 1;
	config.depth_bits = 1;
	config.performance_log_interval = 1;

	CHECK(config.validate());
}

TEST_CASE("[GaussianSplatting][Config] SortingStrategyConfig cascading sanitization") {
	SortingStrategyConfig config;

	// Set unreasonable ordering that should be corrected
	config.bitonic_max_elements = 1000000; // Very large bitonic
	config.radix_max_elements = 100;       // Small radix
	config.onesweep_max_elements = 50;     // Tiny onesweep

	config.sanitize();

	// After sanitization, ordering should be enforced
	CHECK(config.radix_max_elements >= config.bitonic_max_elements);
	CHECK(config.onesweep_max_elements >= config.radix_max_elements);
}

} // namespace TestConfigValidation

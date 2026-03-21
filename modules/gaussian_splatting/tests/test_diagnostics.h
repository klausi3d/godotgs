#pragma once

#include "test_macros.h"
#include "../renderer/rendering_diagnostics.h"
#include "../renderer/rendering_error.h"
#include "../renderer/render_types/diagnostics_snapshot.h"
#include "../renderer/gaussian_splat_renderer.h"
#include "../core/gaussian_splat_manager.h"
#include "main/performance.h"

TEST_CASE("[Gaussian Diagnostics] Singleton initialization is idempotent") {
    GaussianRenderingDiagnostics::ensure_singleton();
    GaussianRenderingDiagnostics *first = GaussianRenderingDiagnostics::get_singleton();
    REQUIRE(first != nullptr);

    GaussianRenderingDiagnostics::ensure_singleton();
    GaussianRenderingDiagnostics *second = GaussianRenderingDiagnostics::get_singleton();
    CHECK(second == first);
}

TEST_CASE("[Gaussian Diagnostics] Null renderer notifications are safe no-ops") {
    GaussianRenderingDiagnostics::ensure_singleton();
    GaussianRenderingDiagnostics *diagnostics = GaussianRenderingDiagnostics::get_singleton();
    REQUIRE(diagnostics != nullptr);

    RenderingError error;
    diagnostics->register_renderer(nullptr);
    diagnostics->unregister_renderer(nullptr);
    diagnostics->notify_error(nullptr, error);
    diagnostics->notify_recovery(nullptr, error);
    diagnostics->notify_frame_completed(nullptr);
    diagnostics->request_runtime_report();

    CHECK(true);
}

// ---------------------------------------------------------------------------
// DiagnosticsSnapshot contract tests
// ---------------------------------------------------------------------------

TEST_CASE("[Gaussian Diagnostics] Snapshot clear resets all fields to defaults") {
    GaussianSplatDiagnosticsSnapshot snapshot;

    // Populate with non-default values across every section.
    snapshot.valid = true;
    snapshot.stage_metrics_valid = true;
    snapshot.pipeline_frame_time_ms = 5.0f;
    snapshot.pipeline_cull_time_ms = 1.0f;
    snapshot.pipeline_sort_time_ms = 2.0f;
    snapshot.pipeline_binning_time_ms = 0.5f;
    snapshot.pipeline_prefix_time_ms = 0.3f;
    snapshot.pipeline_raster_time_ms = 1.5f;
    snapshot.pipeline_resolve_time_ms = 0.2f;
    snapshot.pipeline_composite_time_ms = 0.8f;
    snapshot.cpu_setup_time_ms = 0.1f;
    snapshot.cpu_sort_submit_ms = 0.4f;
    snapshot.cpu_sort_wait_ms = 0.6f;
    snapshot.cpu_sort_input_build_ms = 0.3f;
    snapshot.sort_used_gpu = true;
    snapshot.sort_used_cpu_fallback = true;
    snapshot.sort_algorithm = StringName("BitonicGPU");
    snapshot.sort_element_count = 10000;
    snapshot.visible_splat_count = 5000;
    snapshot.total_processed = 8000;
    snapshot.projection_success_count = 4500;
    snapshot.projection_success_rate_pct = 90.0f;
    snapshot.clip_reject_count = 100;
    snapshot.radius_reject_count = 50;
    snapshot.viewport_reject_count = 25;
    snapshot.extreme_aspect_count = 10;
    snapshot.index_mismatch_count = 5;
    snapshot.tile_count = 120;
    snapshot.overflow_tile_count = 3;
    snapshot.clamped_records = 7;
    snapshot.aggregated_count = 200;
    snapshot.overlap_records_used = 1500;
    snapshot.overlap_record_budget = 4096;
    snapshot.stage_cull_candidate_count = 8000;
    snapshot.stage_cull_visible_count = 5000;
    snapshot.stage_sort_did_sort = true;
    snapshot.stage_sort_input_count = 5000;
    snapshot.stage_sort_sorted_count = 5000;
    snapshot.stage_raster_reused_cached = true;
    snapshot.stage_raster_painterly_active = true;
    snapshot.stage_composite_executed = true;
    snapshot.frame_index = 42;
    snapshot.frame_time_ms = 16.6f;
    snapshot.telemetry_active = true;
    snapshot.route_uid = "test-route";
    snapshot.data_source = "TestSource";

    snapshot.clear();

    // Validity flags must be false.
    CHECK_MESSAGE(!snapshot.valid, "clear() must set valid to false");
    CHECK_MESSAGE(!snapshot.stage_metrics_valid, "clear() must set stage_metrics_valid to false");

    // Pipeline timing must all be zero.
    CHECK(snapshot.pipeline_frame_time_ms == 0.0f);
    CHECK(snapshot.pipeline_cull_time_ms == 0.0f);
    CHECK(snapshot.pipeline_sort_time_ms == 0.0f);
    CHECK(snapshot.pipeline_binning_time_ms == 0.0f);
    CHECK(snapshot.pipeline_prefix_time_ms == 0.0f);
    CHECK(snapshot.pipeline_raster_time_ms == 0.0f);
    CHECK(snapshot.pipeline_resolve_time_ms == 0.0f);
    CHECK(snapshot.pipeline_composite_time_ms == 0.0f);

    // CPU timing must all be zero.
    CHECK(snapshot.cpu_setup_time_ms == 0.0f);
    CHECK(snapshot.cpu_sort_submit_ms == 0.0f);
    CHECK(snapshot.cpu_sort_wait_ms == 0.0f);
    CHECK(snapshot.cpu_sort_input_build_ms == 0.0f);

    // Sort metadata.
    CHECK(!snapshot.sort_used_gpu);
    CHECK(!snapshot.sort_used_cpu_fallback);
    CHECK(snapshot.sort_algorithm == StringName());
    CHECK(snapshot.sort_element_count == 0);

    // Visibility / projection.
    CHECK(snapshot.visible_splat_count == 0);
    CHECK(snapshot.total_processed == 0);
    CHECK(snapshot.projection_success_count == 0);
    CHECK(snapshot.projection_success_rate_pct == 0.0f);
    CHECK(snapshot.clip_reject_count == 0);
    CHECK(snapshot.radius_reject_count == 0);
    CHECK(snapshot.viewport_reject_count == 0);
    CHECK(snapshot.extreme_aspect_count == 0);
    CHECK(snapshot.index_mismatch_count == 0);

    // Tile stats.
    CHECK(snapshot.tile_count == 0);
    CHECK(snapshot.overflow_tile_count == 0);
    CHECK(snapshot.clamped_records == 0);
    CHECK(snapshot.aggregated_count == 0);
    CHECK(snapshot.overlap_records_used == 0);
    CHECK(snapshot.overlap_record_budget == 0);

    // Stage metrics.
    CHECK(snapshot.stage_cull_candidate_count == 0);
    CHECK(snapshot.stage_cull_visible_count == 0);
    CHECK(!snapshot.stage_sort_did_sort);
    CHECK(snapshot.stage_sort_input_count == 0);
    CHECK(snapshot.stage_sort_sorted_count == 0);
    CHECK(!snapshot.stage_raster_reused_cached);
    CHECK(!snapshot.stage_raster_painterly_active);
    CHECK(!snapshot.stage_composite_executed);

    // Frame metadata.
    CHECK(snapshot.frame_index == 0);
    CHECK(snapshot.frame_time_ms == 0.0f);
    CHECK(!snapshot.telemetry_active);
    CHECK(snapshot.route_uid == String());
    CHECK(snapshot.data_source == String());
}

TEST_CASE("[Gaussian Diagnostics] Snapshot to_dictionary exports every field") {
    GaussianSplatDiagnosticsSnapshot snapshot;
    Dictionary d = snapshot.to_dictionary();

    // The snapshot has exactly 46 fields exported to the dictionary.
    CHECK_MESSAGE(d.size() == 46,
            vformat("Expected 46 keys in to_dictionary(), got %d", d.size()));

    // Pipeline stage timing keys.
    CHECK(d.has("pipeline_frame_time_ms"));
    CHECK(d.has("pipeline_cull_time_ms"));
    CHECK(d.has("pipeline_sort_time_ms"));
    CHECK(d.has("pipeline_binning_time_ms"));
    CHECK(d.has("pipeline_prefix_time_ms"));
    CHECK(d.has("pipeline_raster_time_ms"));
    CHECK(d.has("pipeline_resolve_time_ms"));
    CHECK(d.has("pipeline_composite_time_ms"));

    // CPU timing keys.
    CHECK(d.has("cpu_setup_time_ms"));
    CHECK(d.has("cpu_sort_submit_ms"));
    CHECK(d.has("cpu_sort_wait_ms"));
    CHECK(d.has("cpu_sort_input_build_ms"));

    // Sort metadata keys.
    CHECK(d.has("sort_used_gpu"));
    CHECK(d.has("sort_used_cpu_fallback"));
    CHECK(d.has("sort_algorithm"));
    CHECK(d.has("sort_element_count"));

    // Visibility / projection keys.
    CHECK(d.has("visible_splat_count"));
    CHECK(d.has("total_processed"));
    CHECK(d.has("projection_success_count"));
    CHECK(d.has("projection_success_rate_pct"));
    CHECK(d.has("clip_reject_count"));
    CHECK(d.has("radius_reject_count"));
    CHECK(d.has("viewport_reject_count"));
    CHECK(d.has("extreme_aspect_count"));
    CHECK(d.has("index_mismatch_count"));

    // Tile stats keys (including new overlap monitors).
    CHECK(d.has("tile_count"));
    CHECK(d.has("overflow_tile_count"));
    CHECK(d.has("clamped_records"));
    CHECK(d.has("aggregated_count"));
    CHECK(d.has("overlap_records_used"));
    CHECK(d.has("overlap_record_budget"));

    // Stage metrics keys.
    CHECK(d.has("stage_cull_candidate_count"));
    CHECK(d.has("stage_cull_visible_count"));
    CHECK(d.has("stage_sort_did_sort"));
    CHECK(d.has("stage_sort_input_count"));
    CHECK(d.has("stage_sort_sorted_count"));
    CHECK(d.has("stage_raster_reused_cached"));
    CHECK(d.has("stage_raster_painterly_active"));
    CHECK(d.has("stage_composite_executed"));

    // Frame metadata keys.
    CHECK(d.has("frame_index"));
    CHECK(d.has("frame_time_ms"));
    CHECK(d.has("telemetry_active"));
    CHECK(d.has("route_uid"));
    CHECK(d.has("data_source"));

    // Validity keys.
    CHECK(d.has("valid"));
    CHECK(d.has("stage_metrics_valid"));
}

TEST_CASE("[Gaussian Diagnostics] Snapshot to_dictionary returns populated values") {
    GaussianSplatDiagnosticsSnapshot snapshot;

    // Populate representative fields from each section.
    snapshot.valid = true;
    snapshot.pipeline_sort_time_ms = 1.25f;
    snapshot.pipeline_composite_time_ms = 0.75f;
    snapshot.cpu_sort_submit_ms = 0.33f;
    snapshot.cpu_sort_wait_ms = 0.44f;
    snapshot.cpu_sort_input_build_ms = 0.11f;
    snapshot.visible_splat_count = 42000;
    snapshot.overlap_records_used = 2048;
    snapshot.overlap_record_budget = 8192;
    snapshot.frame_index = 99;
    snapshot.telemetry_active = true;
    snapshot.sort_used_gpu = true;
    snapshot.data_source = "StreamingGPU";
    snapshot.stage_sort_did_sort = true;
    snapshot.stage_composite_executed = true;

    Dictionary d = snapshot.to_dictionary();

    CHECK(bool(d["valid"]) == true);
    CHECK(float(d["pipeline_sort_time_ms"]) == doctest::Approx(1.25f));
    CHECK(float(d["pipeline_composite_time_ms"]) == doctest::Approx(0.75f));
    CHECK(float(d["cpu_sort_submit_ms"]) == doctest::Approx(0.33f));
    CHECK(float(d["cpu_sort_wait_ms"]) == doctest::Approx(0.44f));
    CHECK(float(d["cpu_sort_input_build_ms"]) == doctest::Approx(0.11f));
    CHECK(int(d["visible_splat_count"]) == 42000);
    CHECK(int(d["overlap_records_used"]) == 2048);
    CHECK(int(d["overlap_record_budget"]) == 8192);
    CHECK(int64_t(d["frame_index"]) == 99);
    CHECK(bool(d["telemetry_active"]) == true);
    CHECK(bool(d["sort_used_gpu"]) == true);
    CHECK(String(d["data_source"]) == "StreamingGPU");
    CHECK(bool(d["stage_sort_did_sort"]) == true);
    CHECK(bool(d["stage_composite_executed"]) == true);
}

TEST_CASE("[Gaussian Diagnostics] Snapshot clear then to_dictionary returns all-zero defaults") {
    GaussianSplatDiagnosticsSnapshot snapshot;

    // Populate, then clear to verify round-trip back to defaults.
    snapshot.valid = true;
    snapshot.pipeline_frame_time_ms = 10.0f;
    snapshot.cpu_sort_submit_ms = 2.5f;
    snapshot.visible_splat_count = 99999;
    snapshot.overlap_records_used = 4096;
    snapshot.overlap_record_budget = 16384;
    snapshot.frame_index = 500;
    snapshot.telemetry_active = true;
    snapshot.data_source = "Test";

    snapshot.clear();

    Dictionary d = snapshot.to_dictionary();

    CHECK(bool(d["valid"]) == false);
    CHECK(float(d["pipeline_frame_time_ms"]) == 0.0f);
    CHECK(float(d["pipeline_sort_time_ms"]) == 0.0f);
    CHECK(float(d["pipeline_composite_time_ms"]) == 0.0f);
    CHECK(float(d["cpu_sort_submit_ms"]) == 0.0f);
    CHECK(float(d["cpu_sort_wait_ms"]) == 0.0f);
    CHECK(float(d["cpu_sort_input_build_ms"]) == 0.0f);
    CHECK(int(d["visible_splat_count"]) == 0);
    CHECK(int(d["overlap_records_used"]) == 0);
    CHECK(int(d["overlap_record_budget"]) == 0);
    CHECK(int64_t(d["frame_index"]) == 0);
    CHECK(bool(d["telemetry_active"]) == false);
    CHECK(String(d["data_source"]) == String());
    CHECK(bool(d["stage_metrics_valid"]) == false);
}

TEST_CASE("[Gaussian Diagnostics] New monitors are registered in Performance singleton") {
    RenderingServer *rs = RenderingServer::get_singleton();
    if (rs == nullptr) {
        MESSAGE("Skipping test - Rendering server unavailable");
        return;
    }

    GaussianSplatManager *manager = GaussianSplatManager::get_singleton();
    bool owns_manager = false;
    if (!manager) {
        manager = memnew(GaussianSplatManager);
        owns_manager = true;
    }
    if (manager == nullptr) {
        MESSAGE("Skipping test - GaussianSplatManager unavailable");
        return;
    }

    RenderingDevice *rd = manager->get_primary_rendering_device();
    bool owns_rd = false;
    if (!rd) {
        rd = rs->create_local_rendering_device();
        owns_rd = true;
    }
    if (rd == nullptr) {
        MESSAGE("Skipping test - Rendering device unavailable");
        if (owns_manager) {
            memdelete(manager);
        }
        return;
    }

    Ref<GaussianSplatRenderer> renderer;
    renderer.instantiate(rd);
    CHECK(renderer.is_valid());
    if (!renderer.is_valid()) {
        if (owns_rd) {
            memdelete(rd);
        }
        if (owns_manager) {
            memdelete(manager);
        }
        return;
    }

    Performance *perf = Performance::get_singleton();
    CHECK(perf != nullptr);
    if (perf == nullptr) {
        renderer.unref();
        if (owns_rd) {
            memdelete(rd);
        }
        if (owns_manager) {
            memdelete(manager);
        }
        return;
    }

    // Verify all 7 new monitors added in the diagnostics reset are registered.
    // 5 new timing monitors:
    CHECK_MESSAGE(perf->has_custom_monitor("gaussian_splatting/pipeline_sort_time_ms"),
            "Expected pipeline_sort_time_ms monitor to be registered");
    CHECK_MESSAGE(perf->has_custom_monitor("gaussian_splatting/pipeline_composite_time_ms"),
            "Expected pipeline_composite_time_ms monitor to be registered");
    CHECK_MESSAGE(perf->has_custom_monitor("gaussian_splatting/cpu_sort_submit_ms"),
            "Expected cpu_sort_submit_ms monitor to be registered");
    CHECK_MESSAGE(perf->has_custom_monitor("gaussian_splatting/cpu_sort_wait_ms"),
            "Expected cpu_sort_wait_ms monitor to be registered");
    CHECK_MESSAGE(perf->has_custom_monitor("gaussian_splatting/cpu_sort_input_build_ms"),
            "Expected cpu_sort_input_build_ms monitor to be registered");

    // 2 new tile overlap monitors:
    CHECK_MESSAGE(perf->has_custom_monitor("gaussian_splatting/overlap_records_used"),
            "Expected overlap_records_used monitor to be registered");
    CHECK_MESSAGE(perf->has_custom_monitor("gaussian_splatting/overlap_record_budget"),
            "Expected overlap_record_budget monitor to be registered");

    renderer.unref();

    if (owns_rd) {
        memdelete(rd);
    }
    if (owns_manager) {
        memdelete(manager);
    }
}

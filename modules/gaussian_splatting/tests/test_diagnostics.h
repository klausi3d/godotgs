#pragma once

#include "gs_test_setting_guard.h"
#include "test_macros.h"
#include "../renderer/gaussian_splat_renderer.h"
#include "../renderer/render_debug_state_orchestrator.h"
#include "../renderer/render_diagnostics_orchestrator.h"
#include "../renderer/rendering_diagnostics.h"
#include "../renderer/rendering_error.h"

namespace {

bool diagnostics_contract_has_key(const Array &p_contract, const String &p_key) {
    for (int i = 0; i < p_contract.size(); i++) {
        if (String(p_contract[i]) == p_key) {
            return true;
        }
    }
    return false;
}

void diagnostics_require_key(const Dictionary &p_dict, const char *p_key) {
    CHECK_MESSAGE(p_dict.has(p_key), vformat("Expected diagnostics dictionary key '%s'", p_key));
}

} // namespace

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

TEST_CASE("[Gaussian Diagnostics] Production metrics contract exposes GPU timing capture fields without GPU") {
    Ref<GaussianSplatRenderer> renderer;
    renderer.instantiate();
    REQUIRE(renderer.is_valid());
    REQUIRE(renderer->debug_state_orchestrator != nullptr);

    RenderDiagnosticsOrchestrator::Dependencies dependencies;
    dependencies.renderer = renderer.ptr();
    dependencies.debug_state_orchestrator = renderer->debug_state_orchestrator.get();
    dependencies.build_device_capability_report = []() {
        Dictionary report;
        report["test_device_report"] = true;
        return report;
    };
    dependencies.runtime_ports.update_gpu_pass_metrics_from_tile_renderer =
            &GaussianSplatRenderer::clear_debug_overlay_dirty_flags;

    RenderDiagnosticsOrchestrator diagnostics(dependencies);

    Dictionary snapshot = diagnostics.get_runtime_diagnostic_snapshot();
    Array contract = snapshot.get("production_metrics_contract", Array());
    const char *expected_keys[] = {
        "gpu_frame_ms",
        "gpu_binning_ms",
        "gpu_prefix_ms",
        "gpu_raster_ms",
        "gpu_resolve_ms",
        "gpu_timing_frame_serial",
        "gpu_timing_frames_behind",
        "gpu_pass_breakdown_available",
        "raster_path_reason",
        "raster_compute_allowed",
        "raster_total_tiles",
        "raster_empty_tiles",
        "raster_overflow_tiles",
        "raster_max_splats_per_tile",
        "raster_avg_splats_per_tile",
        "raster_occupancy_ratio",
        "raster_dense_ratio",
        "raster_overlap_records",
        "raster_overlap_record_budget_effective",
        "raster_overlap_thinning_keep_ratio",
        "raster_feature_global_sort",
        "raster_feature_packed_stage_data",
        "raster_feature_tighter_bounds",
        "raster_feature_sh_amortization",
        "raster_feature_quantized_storage",
        "raster_feature_debug_counters",
        "raster_tile_splat_capacity",
        "raster_max_raster_splats_per_tile",
        "raster_shader_defines_hash",
        "route_uid",
        "sort_route_uid",
        "cull_route_uid",
        "cull_route_reason",
    };

    for (const char *key : expected_keys) {
        CHECK_MESSAGE(diagnostics_contract_has_key(contract, key),
                vformat("Expected production metrics contract to include '%s'", key));
    }
}

TEST_CASE("[Gaussian Diagnostics] Production metrics preserve GPU timing capture semantics without GPU") {
    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    if (project_settings == nullptr) {
        MESSAGE("Skipping test - ProjectSettings unavailable");
        return;
    }
    OS *os = OS::get_singleton();
    if (os == nullptr) {
        MESSAGE("Skipping test - OS unavailable");
        return;
    }

    const String validate_setting = "rendering/gaussian_splatting/diagnostics/validate_production_metrics";
    const String summary_interval_setting = "rendering/gaussian_splatting/diagnostics/summary_interval_frames";
    const String summary_history_setting = "rendering/gaussian_splatting/diagnostics/summary_history_size";
    const String gate_enabled_setting = "rendering/gaussian_splatting/diagnostics/perf_gate_enabled";
    ProjectSettingGuard validate_guard(project_settings, validate_setting);
    ProjectSettingGuard summary_interval_guard(project_settings, summary_interval_setting);
    ProjectSettingGuard summary_history_guard(project_settings, summary_history_setting);
    ProjectSettingGuard gate_enabled_guard(project_settings, gate_enabled_setting);
    project_settings->set_setting(validate_setting, true);
    project_settings->set_setting(summary_interval_setting, 1);
    project_settings->set_setting(summary_history_setting, 2);
    project_settings->set_setting(gate_enabled_setting, false);

    Ref<GaussianSplatRenderer> renderer;
    renderer.instantiate();
    REQUIRE(renderer.is_valid());
    REQUIRE(renderer->debug_state_orchestrator != nullptr);

    Ref<GaussianData> data;
    data.instantiate();
    data->resize(4096);
    renderer->get_scene_state().gaussian_data = data;

    GaussianSplatRenderer::FrameState &frame_state = renderer->get_frame_state();
    frame_state.frame_counter = 42;
    frame_state.render_time_ms = 1.75f;
    frame_state.sort_time_ms = 0.50f;
    frame_state.visible_splat_count.store(2048, std::memory_order_release);

    GaussianSplatRenderer::PerformanceMetrics &perf = renderer->get_performance_state().metrics;
    perf.data_source = "diagnostics_test";
    perf.raster_path = "tile";
    perf.raster_path_reason = "Compute raster disabled by pipeline settings";
    perf.raster_compute_allowed = false;
    perf.raster_total_tiles = 100;
    perf.raster_empty_tiles = 20;
    perf.raster_overflow_tiles = 3;
    perf.raster_max_splats_per_tile = 4096;
    perf.raster_avg_splats_per_tile = 128.0f;
    perf.raster_occupancy_ratio = 0.80f;
    perf.raster_dense_ratio = 0.25f;
    perf.raster_overlap_records = 300000;
    perf.raster_overlap_record_budget = 400000;
    perf.raster_overlap_record_budget_effective = 350000;
    perf.raster_overlap_record_budget_configured = 400000;
    perf.raster_overlap_thinning_keep_ratio = 0.875f;
    perf.raster_feature_global_sort = true;
    perf.raster_feature_packed_stage_data = false;
    perf.raster_feature_tighter_bounds = true;
    perf.raster_feature_sh_amortization = false;
    perf.raster_sh_amortization_divisor = 1;
    perf.raster_feature_quantized_storage = true;
    perf.raster_feature_debug_counters = false;
    perf.raster_tile_splat_capacity = 1024;
    perf.raster_max_raster_splats_per_tile = 8192;
    perf.raster_shader_defines_hash = 12345;
    perf.cull_route_uid = RenderRouteUID::INSTANCE_CULL_GPU;
    perf.cull_route_reason = "gpu_culler";
    perf.gpu_frame_time_ms = 3.50f;
    perf.gpu_tile_binning_time_ms = 0.40f;
    perf.gpu_tile_prefix_time_ms = 0.30f;
    perf.gpu_tile_raster_time_ms = 2.10f;
    perf.gpu_tile_resolve_time_ms = 0.20f;
    perf.gpu_timing_frame_serial = 40;
    perf.gpu_timing_frames_behind = 2;

    GaussianSplatRenderer::DebugState &debug_state = renderer->get_debug_state();
    debug_state.route_uid = RenderRouteUID::INSTANCE_RASTER_COMPUTE;
    debug_state.sort_route_uid = RenderRouteUID::INSTANCE_SORT_GPU;
    debug_state.last_stage_metrics_valid = true;
    GaussianSplatRenderer::StageMetrics &stage_metrics = debug_state.last_stage_metrics;
    stage_metrics.cull.has_visible = true;
    stage_metrics.cull.visible_count = 2048;
    stage_metrics.cull.candidate_count = 4096;
    stage_metrics.cull.cull_time_ms = 0.25f;
    stage_metrics.cull.visible_domain = GaussianRenderState::IndexDomain::GAUSSIAN_GLOBAL;
    stage_metrics.sort.did_sort = true;
    stage_metrics.sort.input_count = 2048;
    stage_metrics.sort.sorted_count = 2048;
    stage_metrics.sort.sort_time_ms = 0.50f;
    stage_metrics.sort.input_domain = GaussianRenderState::IndexDomain::GAUSSIAN_GLOBAL;
    stage_metrics.sort.output_domain = GaussianRenderState::IndexDomain::GAUSSIAN_GLOBAL;
    stage_metrics.raster.render_time_ms = 1.00f;
    stage_metrics.raster.raster_path = "tile";
    stage_metrics.composite_time_ms = 0.10f;
    stage_metrics.composite_executed = true;

    RenderDiagnosticsOrchestrator::Dependencies dependencies;
    dependencies.renderer = renderer.ptr();
    dependencies.debug_state_orchestrator = renderer->debug_state_orchestrator.get();
    dependencies.build_device_capability_report = []() {
        Dictionary report;
        report["test_device_report"] = true;
        return report;
    };
    dependencies.runtime_ports.update_gpu_pass_metrics_from_tile_renderer =
            &GaussianSplatRenderer::clear_debug_overlay_dirty_flags;

    RenderDiagnosticsOrchestrator diagnostics(dependencies);
    const uint64_t frame_start_usec = os->get_ticks_usec() > 1000 ? os->get_ticks_usec() - 1000 : 0;
    diagnostics.finalize_frame_metrics(frame_start_usec);

    Dictionary snapshot = diagnostics.get_runtime_diagnostic_snapshot();
    Dictionary production_metrics = snapshot.get("production_metrics", Dictionary());
    Dictionary validation = snapshot.get("production_metrics_validation", Dictionary());
    Dictionary telemetry = snapshot.get("telemetry", Dictionary());

    diagnostics_require_key(production_metrics, "gpu_frame_ms");
    diagnostics_require_key(production_metrics, "gpu_binning_ms");
    diagnostics_require_key(production_metrics, "gpu_prefix_ms");
    diagnostics_require_key(production_metrics, "gpu_raster_ms");
    diagnostics_require_key(production_metrics, "gpu_resolve_ms");
    diagnostics_require_key(production_metrics, "gpu_timing_frame_serial");
    diagnostics_require_key(production_metrics, "gpu_timing_frames_behind");
    diagnostics_require_key(production_metrics, "gpu_pass_breakdown_available");
    diagnostics_require_key(production_metrics, "raster_path_reason");
    diagnostics_require_key(production_metrics, "raster_max_splats_per_tile");
    diagnostics_require_key(production_metrics, "raster_overlap_thinning_keep_ratio");
    diagnostics_require_key(production_metrics, "raster_feature_tighter_bounds");
    diagnostics_require_key(production_metrics, "raster_shader_defines_hash");

    CHECK(float(production_metrics.get("gpu_frame_ms", 0.0f)) == doctest::Approx(3.50f));
    CHECK(float(production_metrics.get("gpu_binning_ms", 0.0f)) == doctest::Approx(0.40f));
    CHECK(float(production_metrics.get("gpu_prefix_ms", 0.0f)) == doctest::Approx(0.30f));
    CHECK(float(production_metrics.get("gpu_raster_ms", 0.0f)) == doctest::Approx(2.10f));
    CHECK(float(production_metrics.get("gpu_resolve_ms", 0.0f)) == doctest::Approx(0.20f));
    CHECK(int64_t(production_metrics.get("gpu_timing_frame_serial", int64_t(-1))) == 40);
    CHECK(int64_t(production_metrics.get("gpu_timing_frames_behind", int64_t(-1))) == 2);
    CHECK(bool(production_metrics.get("gpu_pass_breakdown_available", false)));
    CHECK(String(production_metrics.get("raster_path_reason", String())) == "Compute raster disabled by pipeline settings");
    CHECK(int64_t(production_metrics.get("raster_max_splats_per_tile", int64_t(0))) == 4096);
    CHECK(float(production_metrics.get("raster_overlap_thinning_keep_ratio", 0.0f)) == doctest::Approx(0.875f));
    CHECK(bool(production_metrics.get("raster_feature_tighter_bounds", false)));
    CHECK(String(production_metrics.get("raster_shader_defines_hash", String())) == "12345");
    CHECK(bool(validation.get("valid", false)));

    CHECK(float(telemetry.get("gpu_frame_time_ms", 0.0f)) == doctest::Approx(3.50f));
    CHECK(float(telemetry.get("gpu_tile_binning_time_ms", 0.0f)) == doctest::Approx(0.40f));
    CHECK(float(telemetry.get("gpu_tile_prefix_time_ms", 0.0f)) == doctest::Approx(0.30f));
    CHECK(float(telemetry.get("gpu_tile_raster_time_ms", 0.0f)) == doctest::Approx(2.10f));
    CHECK(float(telemetry.get("gpu_tile_resolve_time_ms", 0.0f)) == doctest::Approx(0.20f));
    CHECK(int64_t(telemetry.get("gpu_timing_frame_serial", int64_t(-1))) == 40);
    CHECK(int64_t(telemetry.get("gpu_timing_frames_behind", int64_t(-1))) == 2);
    CHECK(int64_t(telemetry.get("raster_overlap_records", int64_t(0))) == 300000);
    CHECK(bool(telemetry.get("raster_feature_quantized_storage", false)));

    Array summaries = snapshot.get("production_metrics_summaries", Array());
    REQUIRE(summaries.size() == 1);
    Dictionary summary = summaries[0];
    CHECK(summary.has("avg_stage_total_ms"));
    CHECK(int64_t(summary.get("frame_count", int64_t(0))) == 1);
}

/**************************************************************************/
/*  test_streaming_queue_pressure_controller.h                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "../core/streaming_queue_pressure_controller.h"
#include "tests/test_macros.h"

namespace TestGaussianSplatting {

TEST_CASE("[Streaming Pipeline][QueuePressure] Cap-only reason mapping stays stable") {
    StreamingQueuePressureController::PressureSample upload_frame_cap_sample;
    upload_frame_cap_sample.upload_frame_cap_hit = true;
    const StreamingQueuePressureController::PressureSummary upload_frame_cap_summary =
            StreamingQueuePressureController::summarize(upload_frame_cap_sample);
    CHECK(upload_frame_cap_summary.active);
    CHECK(upload_frame_cap_summary.source == String(StreamingQueuePressureController::SOURCE_UPLOAD));
    CHECK(upload_frame_cap_summary.reason == String(StreamingQueuePressureController::REASON_UPLOAD_FRAME_CAP));
    CHECK(StreamingQueuePressureController::validate_summary_invariants(upload_frame_cap_summary, upload_frame_cap_sample));

    StreamingQueuePressureController::PressureSample upload_bandwidth_cap_sample;
    upload_bandwidth_cap_sample.upload_bandwidth_cap_hit = true;
    const StreamingQueuePressureController::PressureSummary upload_bandwidth_cap_summary =
            StreamingQueuePressureController::summarize(upload_bandwidth_cap_sample);
    CHECK(upload_bandwidth_cap_summary.active);
    CHECK(upload_bandwidth_cap_summary.source == String(StreamingQueuePressureController::SOURCE_UPLOAD));
    CHECK(upload_bandwidth_cap_summary.reason == String(StreamingQueuePressureController::REASON_UPLOAD_BANDWIDTH_CAP));
    CHECK(StreamingQueuePressureController::validate_summary_invariants(upload_bandwidth_cap_summary, upload_bandwidth_cap_sample));

    StreamingQueuePressureController::PressureSample chunk_load_cap_sample;
    chunk_load_cap_sample.chunk_load_cap_hit = true;
    const StreamingQueuePressureController::PressureSummary chunk_load_cap_summary =
            StreamingQueuePressureController::summarize(chunk_load_cap_sample);
    CHECK(chunk_load_cap_summary.active);
    CHECK(chunk_load_cap_summary.source == String(StreamingQueuePressureController::SOURCE_UPLOAD));
    CHECK(chunk_load_cap_summary.reason == String(StreamingQueuePressureController::REASON_CHUNK_LOAD_CAP));
    CHECK(StreamingQueuePressureController::validate_summary_invariants(chunk_load_cap_summary, chunk_load_cap_sample));

    StreamingQueuePressureController::PressureSample pack_inflight_cap_sample;
    pack_inflight_cap_sample.pack_inflight_saturated = true;
    const StreamingQueuePressureController::PressureSummary pack_inflight_cap_summary =
            StreamingQueuePressureController::summarize(pack_inflight_cap_sample);
    CHECK(pack_inflight_cap_summary.active);
    CHECK(pack_inflight_cap_summary.source == String(StreamingQueuePressureController::SOURCE_PACK));
    CHECK(pack_inflight_cap_summary.reason == String(StreamingQueuePressureController::REASON_PACK_INFLIGHT_CAP));
    CHECK(StreamingQueuePressureController::validate_summary_invariants(pack_inflight_cap_summary, pack_inflight_cap_sample));

    StreamingQueuePressureController::PressureSample sync_backpressure_sample;
    sync_backpressure_sample.sync_backpressure = true;
    const StreamingQueuePressureController::PressureSummary sync_backpressure_summary =
            StreamingQueuePressureController::summarize(sync_backpressure_sample);
    CHECK(sync_backpressure_summary.active);
    CHECK(sync_backpressure_summary.source == String(StreamingQueuePressureController::SOURCE_SYNC));
    CHECK(sync_backpressure_summary.reason == String(StreamingQueuePressureController::REASON_SYNC_FALLBACK_PRESSURE));
    CHECK(StreamingQueuePressureController::validate_summary_invariants(sync_backpressure_summary, sync_backpressure_sample));

    StreamingQueuePressureController::PressureSample vram_cap_sample;
    vram_cap_sample.vram_chunk_cap_hit = true;
    const StreamingQueuePressureController::PressureSummary vram_cap_summary =
            StreamingQueuePressureController::summarize(vram_cap_sample);
    CHECK(vram_cap_summary.active);
    CHECK(vram_cap_summary.source == String(StreamingQueuePressureController::SOURCE_CAP));
    CHECK(vram_cap_summary.reason == String(StreamingQueuePressureController::REASON_VRAM_CHUNK_CAP));
    CHECK(StreamingQueuePressureController::validate_summary_invariants(vram_cap_summary, vram_cap_sample));

    StreamingQueuePressureController::PressureSample combined_upload_caps_sample;
    combined_upload_caps_sample.upload_frame_cap_hit = true;
    combined_upload_caps_sample.upload_bandwidth_cap_hit = true;
    const StreamingQueuePressureController::PressureSummary combined_upload_caps_summary =
            StreamingQueuePressureController::summarize(combined_upload_caps_sample);
    CHECK(combined_upload_caps_summary.active);
    CHECK(combined_upload_caps_summary.source == String(StreamingQueuePressureController::SOURCE_UPLOAD));
    CHECK(combined_upload_caps_summary.reason == String(StreamingQueuePressureController::REASON_CAP_COMBINED));
    CHECK(StreamingQueuePressureController::validate_summary_invariants(combined_upload_caps_summary, combined_upload_caps_sample));
}

TEST_CASE("[Streaming Pipeline][QueuePressure] Helper boundaries stay deterministic") {
    bool latch_active = false;
    String latch_source = "invalid";
    String latch_reason = "invalid";
    StreamingQueuePressureController::mark_latched_state(latch_active, latch_source, latch_reason, nullptr, nullptr);
    CHECK(latch_active);
    CHECK(latch_source == String(StreamingQueuePressureController::SOURCE_CAP));
    CHECK(latch_reason == String(StreamingQueuePressureController::REASON_CAP_COMBINED));
    CHECK(StreamingQueuePressureController::validate_latched_state_invariants(latch_active, latch_source, latch_reason));

    StreamingQueuePressureController::mark_latched_state(latch_active, latch_source, latch_reason, "bogus_source", "bogus_reason");
    CHECK(latch_active);
    CHECK(latch_source == String(StreamingQueuePressureController::SOURCE_CAP));
    CHECK(latch_reason == String(StreamingQueuePressureController::REASON_CAP_COMBINED));
    CHECK(StreamingQueuePressureController::validate_latched_state_invariants(latch_active, latch_source, latch_reason));

    String latch_error;
    CHECK_FALSE(StreamingQueuePressureController::validate_latched_state_invariants(
            true,
            String(StreamingQueuePressureController::SOURCE_NONE),
            String(StreamingQueuePressureController::REASON_NONE),
            &latch_error));
    CHECK_FALSE(latch_error.is_empty());
}

TEST_CASE("[Streaming Pipeline][QueuePressure] Scan-budget boundary conditions stay stable") {
    StreamingQueuePressureController::ScanBudgetInput exhausted_input;
    exhausted_input.base_scan_budget = 7;
    exhausted_input.throttle_enabled = true;
    exhausted_input.throttle_min_queue_depth = 3;
    exhausted_input.observed_queue_depth = 3;
    exhausted_input.throttle_scan_cap = 4;
    exhausted_input.scanned_this_frame = 4;
    const StreamingQueuePressureController::ScanBudgetResult exhausted_result =
            StreamingQueuePressureController::compute_candidate_scan_budget(exhausted_input);
    CHECK(exhausted_result.throttle_active);
    CHECK(exhausted_result.scan_budget == 0);

    StreamingQueuePressureController::ScanBudgetInput zero_base_input = exhausted_input;
    zero_base_input.base_scan_budget = 0;
    zero_base_input.scanned_this_frame = 0;
    zero_base_input.enqueue_headroom = 0;
    const StreamingQueuePressureController::ScanBudgetResult zero_base_result =
            StreamingQueuePressureController::compute_candidate_scan_budget(zero_base_input);
    CHECK_FALSE(zero_base_result.throttle_active);
    CHECK(zero_base_result.scan_budget == 0);
    CHECK(zero_base_result.effective_queue_depth == zero_base_input.observed_queue_depth);

    StreamingQueuePressureController::ScanBudgetInput constrained_headroom_input;
    constrained_headroom_input.base_scan_budget = 6;
    constrained_headroom_input.throttle_enabled = true;
    constrained_headroom_input.throttle_min_queue_depth = 4;
    constrained_headroom_input.observed_queue_depth = 0;
    constrained_headroom_input.throttle_scan_cap = 8;
    constrained_headroom_input.scanned_this_frame = 0;
    constrained_headroom_input.enqueue_headroom = 2;
    const StreamingQueuePressureController::ScanBudgetResult constrained_headroom_result =
            StreamingQueuePressureController::compute_candidate_scan_budget(constrained_headroom_input);
    CHECK(constrained_headroom_result.throttle_active);
    CHECK(constrained_headroom_result.effective_queue_depth == 6);
    CHECK(constrained_headroom_result.scan_budget == 2);
}

} // namespace TestGaussianSplatting

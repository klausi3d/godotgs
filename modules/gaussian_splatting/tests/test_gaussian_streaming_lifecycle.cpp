#define _ALLOW_KEYWORD_MACROS
#define private public
#include "../core/gaussian_streaming.h"
#undef private

#include "test_macros.h"

#include "core/os/os.h"
#include "servers/rendering_server.h"

namespace {

Ref<GaussianData> _create_streaming_phase_order_test_data(uint32_t p_count = 1024) {
    Ref<GaussianData> data;
    data.instantiate();

    LocalVector<Gaussian> gaussians;
    gaussians.resize(p_count);

    const uint32_t grid_width = 32;
    for (uint32_t i = 0; i < p_count; i++) {
        Gaussian &g = gaussians[i];
        const float x = float(i % grid_width) * 0.05f;
        const float y = float((i / grid_width) % grid_width) * 0.05f;
        const float z = -2.0f - float(i / (grid_width * grid_width)) * 0.05f;
        g.position = Vector3(x, y, z);
        g.scale = Vector3(0.05f, 0.05f, 0.05f);
        g.rotation = Quaternion();
        g.opacity = 1.0f;
        g.sh_dc = Color(1.0f, 0.85f, 0.7f, 1.0f);
        g.normal = Vector3(0.0f, 1.0f, 0.0f);
        g.area = 0.01f;
    }

    data->set_gaussians(gaussians);
    return data;
}

struct TestRenderingDeviceHandle {
    RenderingDevice *rd = nullptr;
    bool owns_rd = false;

    ~TestRenderingDeviceHandle() {
        if (owns_rd && rd) {
            memdelete(rd);
        }
    }
};

TestRenderingDeviceHandle _get_test_rendering_device() {
    RenderingServer *rs = RenderingServer::get_singleton();
    if (!rs) {
        return {};
    }

    RenderingDevice *rd = RenderingDevice::get_singleton();
    if (!rd) {
        rd = rs->create_local_rendering_device();
        return { rd, rd != nullptr };
    }
    return { rd, false };
}

GaussianStreamingSystem::StreamingChunk _make_streaming_chunk(
        const Vector3 &p_center,
        bool p_is_loaded = false,
        bool p_upload_pending = false,
        uint32_t p_buffer_slot = UINT32_MAX) {
    GaussianStreamingSystem::StreamingChunk chunk;
    chunk.center = p_center;
    chunk.bounds = AABB(p_center - Vector3(0.5f, 0.5f, 0.5f), Vector3(1.0f, 1.0f, 1.0f));
    chunk.count = 32;
    chunk.effective_count = 32;
    chunk.is_loaded = p_is_loaded;
    chunk.upload_pending = p_upload_pending;
    chunk.buffer_slot = p_buffer_slot;
    return chunk;
}

} // namespace

TEST_CASE("[Streaming Pipeline] stop_pack_threads clears partial lifecycle state") {
    GaussianStreamingSystem system;
    auto &uploads = system._internal_get_upload_pipeline();

    uploads.pack_thread_running.store(false, std::memory_order_release);
    uploads.pack_thread_exit.store(true, std::memory_order_release);
    uploads.pack_threads.resize(2);
    uploads.pack_thread_contexts.resize(2);
    uploads.pack_threads[0] = nullptr;
    uploads.pack_threads[1] = nullptr;

    uploads.stop_pack_threads(system);

    CHECK(uploads.pack_threads.is_empty());
    CHECK(uploads.pack_thread_contexts.is_empty());
    CHECK_FALSE(uploads.pack_thread_running.load(std::memory_order_acquire));
    CHECK_FALSE(uploads.pack_thread_exit.load(std::memory_order_acquire));
}

TEST_CASE("[Streaming Pipeline] sync pack rescue does not steal worker-owned pack jobs") {
    GaussianStreamingSystem system;
    auto &uploads = system._internal_get_upload_pipeline();

    uploads.async_pack_enabled = true;
    uploads.pack_thread_running.store(true, std::memory_order_release);

    {
        MutexLock lock(uploads.pack_mutex);
        uploads.pack_queue.push_back(StreamingUploadPipeline::PackJob());
        uploads.sync_cached_queue_depths_locked();
    }

    CHECK(uploads.promote_pack_jobs_sync(1) == 0);
    CHECK(uploads.get_pack_queue_depth_cached() == 1);
    CHECK(uploads.get_upload_queue_depth_cached() == 0);
}

TEST_CASE("[Streaming Pipeline] async chunk upload rejects tampered payload checksums") {
    const TestRenderingDeviceHandle rd_handle = _get_test_rendering_device();
    RenderingDevice *rd = rd_handle.rd;
    if (!rd) {
        MESSAGE("Skipping - Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);
    if (!system->is_runtime_ready()) {
        MESSAGE("Skipping - Streaming runtime not ready");
        return;
    }

    GaussianStreamingSystem &system_ref = *system.ptr();
    auto &uploads = system->_internal_get_upload_pipeline();
    if (!uploads.async_pack_enabled || !uploads.pack_thread_running.load(std::memory_order_acquire)) {
        MESSAGE("Skipping - Async pack threads unavailable");
        return;
    }

    const uint32_t asset_id = 4242;
    system->register_asset(asset_id, _create_streaming_phase_order_test_data());

    const bool queued_upload = uploads.queue_chunk_load(system_ref, asset_id, 0);
    REQUIRE(queued_upload);

    StreamingUploadPipeline::PendingChunkUpload *prepared_job = nullptr;
    for (int i = 0; i < 500; i++) {
        {
            MutexLock lock(uploads.pack_mutex);
            if (uploads.upload_queue_read_idx < uploads.upload_queue.size()) {
                prepared_job = uploads.upload_queue[uploads.upload_queue_read_idx];
                if (prepared_job && !prepared_job->packed_data.is_empty()) {
                    break;
                }
                prepared_job = nullptr;
            }
        }
        OS::get_singleton()->delay_usec(1000);
    }

    REQUIRE(prepared_job != nullptr);

    {
        MutexLock lock(uploads.pack_mutex);
        prepared_job = uploads.upload_queue[uploads.upload_queue_read_idx];
        REQUIRE(prepared_job != nullptr);
        REQUIRE(!prepared_job->packed_data.is_empty());
        PackedGaussian *packed_data = prepared_job->packed_data.ptrw();
        REQUIRE(packed_data != nullptr);
        uint8_t *payload_bytes = reinterpret_cast<uint8_t *>(packed_data);
        payload_bytes[0] ^= 0x01;
    }

    uploads.process_upload_queue(system_ref);
    system->begin_frame();
    system->end_frame();

    CHECK(system->get_pending_pack_jobs() == 0);
    CHECK(system->get_pending_upload_jobs() == 0);
    CHECK(system->get_loaded_chunks() == 0);

    Dictionary analytics = system->get_streaming_analytics();
    Dictionary diagnostics = analytics.get("diagnostics", Dictionary());
    CHECK(String(analytics.get("diagnostics_category", String())) == "integrity_mismatch");
    CHECK(String(analytics.get("diagnostics_reason", String())).contains("checksum mismatch"));
    CHECK(bool(analytics.get("diagnostics_has_failure", false)));
    CHECK(String(diagnostics.get("category", String())) == "integrity_mismatch");
    CHECK(String(diagnostics.get("reason", String())).contains("checksum mismatch"));
    CHECK(int64_t(diagnostics.get("invariant_upload_lifecycle_violations", int64_t(0))) == 1);
    CHECK(String(diagnostics.get("last_invariant_context", String())) == "process_upload_queue.payload_checksum");
    CHECK(String(diagnostics.get("last_invariant_message", String())).contains("checksum mismatch"));
    CHECK(int64_t(diagnostics.get("integrity_mismatch_count", int64_t(0))) == 1);
    CHECK(String(diagnostics.get("last_integrity_mismatch_message", String())).contains("checksum mismatch"));

    system->initialize_empty(rd);
    system->begin_frame();
    system->end_frame();

    Dictionary reset_analytics = system->get_streaming_analytics();
    Dictionary reset_diagnostics = reset_analytics.get("diagnostics", Dictionary());
    CHECK(String(reset_analytics.get("diagnostics_category", String("ok"))) == "ok");
    CHECK(String(reset_analytics.get("diagnostics_reason", String("healthy"))) == "healthy");
    CHECK_FALSE(bool(reset_analytics.get("diagnostics_has_failure", true)));
    CHECK(int64_t(reset_diagnostics.get("integrity_mismatch_count", int64_t(-1))) == 0);
    CHECK(String(reset_diagnostics.get("last_integrity_mismatch_message", String())).is_empty());
}

TEST_CASE("[Streaming Pipeline] update_streaming publishes phase timings before atlas sync and keeps atlas generation stable when idle") {
    const TestRenderingDeviceHandle rd_handle = _get_test_rendering_device();
    RenderingDevice *rd = rd_handle.rd;
    if (!rd) {
        MESSAGE("Skipping - Rendering device unavailable");
        return;
    }

    {
        Ref<GaussianStreamingSystem> system;
        system.instantiate();
        system->initialize_empty(rd);
        if (!system->is_runtime_ready()) {
            MESSAGE("Skipping - Streaming runtime not ready");
            return;
        }

        const uint32_t asset_id = 31415;
        system->register_asset(asset_id, _create_streaming_phase_order_test_data());

        Transform3D camera_transform;
        camera_transform.origin = Vector3(0.0f, 0.0f, 5.0f);
        Projection projection;
        projection.set_perspective(60.0f, 1.0f, 0.1f, 2000.0f);

        const RID asset_meta_before = system->get_asset_meta_buffer();
        const RID chunk_meta_before = system->get_chunk_meta_buffer();
        const RID asset_chunk_index_before = system->get_asset_chunk_index_buffer();
        const uint64_t generation_before = system->get_atlas_generation();

        system->begin_frame();
        system->update_streaming(camera_transform, projection);
        const uint64_t generation_after_update = system->get_atlas_generation();
        CHECK(generation_after_update > generation_before);
        CHECK(generation_after_update > 0);
        system->end_frame();

        Dictionary analytics = system->get_streaming_analytics();
        CHECK(analytics.has("scheduler_visibility_cpu_ms"));
        CHECK(analytics.has("scheduler_load_cpu_ms"));
        CHECK(analytics.has("scheduler_build_visible_cpu_ms"));
        CHECK(analytics.has("scheduler_prefetch_cpu_ms"));
        CHECK(analytics.has("scheduler_update_cpu_ms"));
        CHECK(analytics.has("scheduler_cpu_total_attributed_ms"));
        CHECK(analytics.has("scheduler_cpu_unattributed_ms"));
        CHECK(analytics.has("atlas_generation"));

        const double visibility_cpu_ms = double(analytics.get("scheduler_visibility_cpu_ms", 0.0));
        const double load_cpu_ms = double(analytics.get("scheduler_load_cpu_ms", 0.0));
        const double build_visible_cpu_ms = double(analytics.get("scheduler_build_visible_cpu_ms", 0.0));
        const double prefetch_cpu_ms = double(analytics.get("scheduler_prefetch_cpu_ms", 0.0));
        const double update_cpu_ms = double(analytics.get("scheduler_update_cpu_ms", 0.0));
        const double attributed_cpu_ms = double(analytics.get("scheduler_cpu_total_attributed_ms", 0.0));
        const double unattributed_cpu_ms = double(analytics.get("scheduler_cpu_unattributed_ms", 0.0));

        CHECK(visibility_cpu_ms >= 0.0);
        CHECK(load_cpu_ms >= 0.0);
        CHECK(build_visible_cpu_ms >= 0.0);
        CHECK(prefetch_cpu_ms >= 0.0);
        CHECK(update_cpu_ms >= 0.0);
        CHECK(attributed_cpu_ms >= 0.0);
        CHECK(unattributed_cpu_ms >= 0.0);
        CHECK(update_cpu_ms + 0.0001 >= attributed_cpu_ms);
        CHECK(attributed_cpu_ms + 0.0001 >= visibility_cpu_ms);
        CHECK(attributed_cpu_ms + 0.0001 >= load_cpu_ms);
        CHECK(attributed_cpu_ms + 0.0001 >= build_visible_cpu_ms);
        CHECK(attributed_cpu_ms + 0.0001 >= prefetch_cpu_ms);

        CHECK(int64_t(analytics.get("atlas_generation", int64_t(-1))) == int64_t(generation_after_update));
        CHECK(system->get_atlas_generation() == generation_after_update);
        CHECK(system->get_asset_meta_buffer().is_valid());
        CHECK(system->get_chunk_meta_buffer().is_valid());
        CHECK(system->get_asset_chunk_index_buffer().is_valid());

        const uint64_t generation_after_first_update = generation_after_update;
        const RID asset_meta_after_first_update = system->get_asset_meta_buffer();
        const RID chunk_meta_after_first_update = system->get_chunk_meta_buffer();
        const RID asset_chunk_index_after_first_update = system->get_asset_chunk_index_buffer();

        system->begin_frame();
        system->update_streaming(camera_transform, projection);
        system->end_frame();

        CHECK(system->get_atlas_generation() == generation_after_first_update);
        CHECK(system->get_asset_meta_buffer().get_id() == asset_meta_after_first_update.get_id());
        CHECK(system->get_chunk_meta_buffer().get_id() == chunk_meta_after_first_update.get_id());
        CHECK(system->get_asset_chunk_index_buffer().get_id() == asset_chunk_index_after_first_update.get_id());
        const bool asset_meta_stable = (system->get_asset_meta_buffer().get_id() == asset_meta_before.get_id()) || (asset_meta_before.get_id() == 0);
        CHECK(asset_meta_stable);
        const bool chunk_meta_stable = (system->get_chunk_meta_buffer().get_id() == chunk_meta_before.get_id()) || (chunk_meta_before.get_id() == 0);
        CHECK(chunk_meta_stable);
        const bool chunk_index_stable = (system->get_asset_chunk_index_buffer().get_id() == asset_chunk_index_before.get_id()) || (asset_chunk_index_before.get_id() == 0);
        CHECK(chunk_index_stable);
    }
}

TEST_CASE("[Streaming Pipeline] initialize_empty republishes atlas state after registry cleanup") {
    const TestRenderingDeviceHandle rd_handle = _get_test_rendering_device();
    RenderingDevice *rd = rd_handle.rd;
    if (!rd) {
        MESSAGE("Skipping - Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);
    if (!system->is_runtime_ready()) {
        MESSAGE("Skipping - Streaming runtime not ready");
        return;
    }

    const uint32_t asset_id = 27182;
    system->register_asset(asset_id, _create_streaming_phase_order_test_data());

    Transform3D camera_transform;
    camera_transform.origin = Vector3(0.0f, 0.0f, 5.0f);
    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 2000.0f);

    system->begin_frame();
    system->update_streaming(camera_transform, projection);
    system->end_frame();

    const uint64_t generation_before_reinit = system->get_atlas_generation();
    CHECK(generation_before_reinit > 0);
    CHECK(system->get_asset_meta_buffer().is_valid());
    CHECK(system->get_chunk_meta_buffer().is_valid());
    CHECK(system->get_asset_chunk_index_buffer().is_valid());

    system->initialize_empty(rd);
    CHECK(system->is_runtime_ready());
    CHECK(system->get_atlas_generation() > generation_before_reinit);
    CHECK(system->get_asset_meta_buffer().is_valid());
    CHECK(system->get_chunk_meta_buffer().is_valid());
    CHECK(system->get_asset_chunk_index_buffer().is_valid());
    CHECK_FALSE(system->has_asset(asset_id));
}

TEST_CASE("[Streaming Pipeline] initialize_empty keeps atlas metadata buffers valid with zero chunks") {
    const TestRenderingDeviceHandle rd_handle = _get_test_rendering_device();
    RenderingDevice *rd = rd_handle.rd;
    if (!rd) {
        MESSAGE("Skipping - Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);
    if (!system->is_runtime_ready()) {
        MESSAGE("Skipping - Streaming runtime not ready");
        return;
    }

    const uint64_t first_generation = system->get_atlas_generation();
    CHECK(first_generation > 0);
    CHECK(system->get_asset_meta_buffer().is_valid());
    CHECK(system->get_chunk_meta_buffer().is_valid());
    CHECK(system->get_asset_chunk_index_buffer().is_valid());

    system->initialize_empty(rd);

    CHECK(system->is_runtime_ready());
    CHECK(system->get_atlas_generation() > first_generation);
    CHECK(system->get_asset_meta_buffer().is_valid());
    CHECK(system->get_chunk_meta_buffer().is_valid());
    CHECK(system->get_asset_chunk_index_buffer().is_valid());
}

TEST_CASE("[Streaming Pipeline] visibility update preserves nearest-first ordering and mixed residency counts") {
    GaussianStreamingSystem system;
    auto &visibility = system.visibility;
    visibility.chunk_frustum_culling_enabled = false;
    visibility.chunk_frustum_padding = 1.0f;
    visibility.chunk_radius_multiplier = 1.0f;

    system.chunks.resize(3);
    system.chunks[0] = _make_streaming_chunk(Vector3(10.0f, 0.0f, 0.0f), true, false, 7);
    system.chunks[1] = _make_streaming_chunk(Vector3(2.0f, 0.0f, 0.0f), true, true, 3);
    system.chunks[2] = _make_streaming_chunk(Vector3(5.0f, 0.0f, 0.0f), false, false, UINT32_MAX);

    Transform3D camera_transform;
    camera_transform.origin = Vector3(0.0f, 0.0f, 0.0f);
    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 100.0f);

    visibility.update_chunk_visibility(system, camera_transform, projection);

    CHECK(visibility.culling_stats.total_chunks == 3);
    CHECK(visibility.culling_stats.visible_chunks == 3);
    CHECK(visibility.culling_stats.frustum_culled_chunks == 0);
    CHECK(visibility.culling_stats.loaded_chunks == 2);
    CHECK(visibility.culling_stats.resident_chunks == 1);
    REQUIRE(visibility.visible_chunk_indices.size() == 3);
    CHECK(visibility.visible_chunk_indices[0] == 1);
    CHECK(visibility.visible_chunk_indices[1] == 2);
    CHECK(visibility.visible_chunk_indices[2] == 0);
    CHECK(system.chunks[0].is_visible);
    CHECK(system.chunks[1].is_visible);
    CHECK(system.chunks[2].is_visible);
    CHECK(system.chunks[1].distance == doctest::Approx(2.0f));
    CHECK(system.chunks[2].distance == doctest::Approx(5.0f));
    CHECK(system.chunks[0].distance == doctest::Approx(10.0f));
}

TEST_CASE("[Streaming Pipeline] zero-visible recovery prefers local spatial-grid fallback") {
    GaussianStreamingSystem system;
    auto &visibility = system.visibility;
    visibility.chunk_frustum_culling_enabled = true;

    system.chunks.resize(3);
    system.chunks[0] = _make_streaming_chunk(Vector3(1.0f, 0.0f, 0.0f), false, false, UINT32_MAX);
    system.chunks[1] = _make_streaming_chunk(Vector3(6.0f, 0.0f, 0.0f), false, false, UINT32_MAX);
    system.chunks[2] = _make_streaming_chunk(Vector3(5005.0f, 0.0f, 0.0f), false, false, UINT32_MAX);

    for (uint32_t i = 0; i < system.chunks.size(); i++) {
        system.chunks[i].is_visible = false;
    }

    visibility.spatial_grid.build(system.chunks.ptr(), system.chunks.size());
    visibility.camera_tracker.has_previous_position = true;
    visibility.camera_tracker.last_position = Vector3(0.0f, 0.0f, 0.0f);
    visibility.culling_stats.total_chunks = system.chunks.size();
    visibility.culling_stats.visible_chunks = 0;
    visibility.culling_stats.frustum_culled_chunks = system.chunks.size();
    visibility.zero_visible_recovery.zero_visible_consecutive_frames = 0;
    visibility.zero_visible_recovery.recoveries_triggered = 0;
    visibility.zero_visible_recovery.last_recovery_frame = UINT64_MAX;
    system.budget.loaded_chunks_count = 0;
    system.total_frame_count = 42;

    visibility.handle_zero_visible_chunk_recovery(system);

    CHECK(visibility.zero_visible_recovery.zero_visible_consecutive_frames == 1);
    CHECK(visibility.zero_visible_recovery.recoveries_triggered == 1);
    CHECK(visibility.zero_visible_recovery.last_recovery_frame == 42);
    REQUIRE(visibility.visible_chunk_indices.size() == 2);
    CHECK(visibility.visible_chunk_indices[0] == 0);
    CHECK(visibility.visible_chunk_indices[1] == 1);
    CHECK(system.chunks[0].is_visible);
    CHECK(system.chunks[1].is_visible);
    CHECK_FALSE(system.chunks[2].is_visible);
    CHECK(visibility.culling_stats.visible_chunks == 2);
    CHECK(visibility.culling_stats.frustum_culled_chunks == 1);
}

TEST_CASE("[Streaming Pipeline] prefetch candidate collection respects scan budget and pending skips") {
    GaussianStreamingSystem system;
    auto &visibility = system.visibility;

    system.chunks.resize(5);
    system.chunks[0] = _make_streaming_chunk(Vector3(1.0f, 0.0f, 0.0f), false, false, UINT32_MAX);
    system.chunks[1] = _make_streaming_chunk(Vector3(2.0f, 0.0f, 0.0f), false, true, 1);
    system.chunks[2] = _make_streaming_chunk(Vector3(3.0f, 0.0f, 0.0f), true, false, 2);
    system.chunks[3] = _make_streaming_chunk(Vector3(30.0f, 0.0f, 0.0f), false, false, UINT32_MAX);
    system.chunks[4] = _make_streaming_chunk(Vector3(4.0f, 0.0f, 0.0f), false, false, UINT32_MAX);

    visibility.prefetch_lookahead_distance = 10.0f;
    system.scheduler.max_prefetch_chunk_scan_per_frame = 5;
    system.scheduler.prefetch_scan_budget_remaining_this_frame = 5;
    system.scheduler.prefetch_scan_cursor = 0;
    system.scheduler.last_prefetch_scan_count = 0;
    system.scheduler.last_prefetch_candidate_count = 0;
    system.scheduler.last_prefetch_upload_pending_skip_count = 0;
    system.scheduler.last_prefetch_scan_budget_effective = 0;
    system.scheduler.queue_pressure_candidate_scan_throttle_enabled = false;

    LocalVector<uint32_t> candidates;
    visibility.collect_prefetch_candidates(system, Vector3(0.0f, 0.0f, 0.0f), 2, 2, candidates);

    REQUIRE(candidates.size() == 2);
    CHECK(candidates[0] == 0);
    CHECK(candidates[1] == UINT32_MAX);
    CHECK(system.scheduler.prefetch_scan_cursor == 2);
    CHECK(system.scheduler.prefetch_scan_budget_remaining_this_frame == 3);
    CHECK(system.scheduler.last_prefetch_scan_count == 2);
    CHECK(system.scheduler.last_prefetch_candidate_count == 1);
    CHECK(system.scheduler.last_prefetch_upload_pending_skip_count == 1);
    CHECK(system.scheduler.last_prefetch_scan_budget_effective == 2);
}

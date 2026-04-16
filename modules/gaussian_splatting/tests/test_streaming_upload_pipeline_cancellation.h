/**************************************************************************/
/*  test_streaming_upload_pipeline_cancellation.h                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "../core/gaussian_streaming.h"
#include "tests/test_macros.h"

#include "servers/rendering_server.h"

namespace TestGaussianSplatting {
namespace {

Ref<GaussianData> _create_upload_pipeline_cancellation_test_data(uint32_t p_count = 1024) {
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

struct UploadPipelineCancellationDeviceHandle {
    RenderingDevice *rd = nullptr;
    bool owns_rd = false;

    ~UploadPipelineCancellationDeviceHandle() {
        if (owns_rd && rd) {
            memdelete(rd);
        }
    }
};

UploadPipelineCancellationDeviceHandle _acquire_upload_pipeline_cancellation_device() {
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

} // namespace

TEST_CASE("[Streaming Pipeline][UploadPipeline] cancel_chunk_jobs honors slot filters and clears pending state") {
    const UploadPipelineCancellationDeviceHandle rd_handle = _acquire_upload_pipeline_cancellation_device();
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

    const uint32_t asset_id = 5100;
    system->register_asset(asset_id, _create_upload_pipeline_cancellation_test_data());

    GaussianStreamingSystem &system_ref = *system.ptr();
    auto &uploads = system->_internal_get_upload_pipeline();
    uploads.stop_pack_threads(system_ref);
    uploads.async_pack_enabled = true;

    REQUIRE(uploads.queue_chunk_load(system_ref, asset_id, 0));
    REQUIRE(system->get_pending_pack_jobs() == 1);
    REQUIRE(uploads.pack_jobs_in_flight.load(std::memory_order_acquire) == 1);

    auto *asset = system->_test_get_asset_state(asset_id);
    REQUIRE(asset != nullptr);
    auto &asset_chunks = system->_test_get_asset_chunks(*asset);
    REQUIRE(asset_chunks.size() > 0);
    auto &chunk = asset_chunks[0];
    REQUIRE(chunk.upload_pending);
    REQUIRE(chunk.buffer_slot != UINT32_MAX);

    const uint32_t reserved_slot = chunk.buffer_slot;
    const uint64_t chunk_key = system->_test_make_chunk_key(asset_id, 0);
    uint32_t mapped_slot = UINT32_MAX;
    REQUIRE(system->_test_atlas_allocator().get_slot(chunk_key, mapped_slot));
    REQUIRE(mapped_slot == reserved_slot);

    const uint32_t mismatch_slot = reserved_slot == 0 ? 1u : 0u;
    uploads.cancel_chunk_jobs(system_ref, asset_id, 0, mismatch_slot);

    CHECK(system->get_pending_pack_jobs() == 1);
    CHECK(uploads.pack_jobs_in_flight.load(std::memory_order_acquire) == 1);
    CHECK(chunk.upload_pending);
    CHECK(chunk.buffer_slot == reserved_slot);
    CHECK(system->_test_atlas_allocator().get_slot(chunk_key, mapped_slot));
    CHECK(mapped_slot == reserved_slot);

    uploads.cancel_chunk_jobs(system_ref, asset_id, 0, reserved_slot);

    CHECK(system->get_pending_pack_jobs() == 0);
    CHECK(system->get_pending_upload_jobs() == 0);
    CHECK(uploads.pack_jobs_in_flight.load(std::memory_order_acquire) == 0);
    CHECK_FALSE(chunk.upload_pending);
    CHECK_FALSE(chunk.is_loaded);
    CHECK(chunk.buffer_slot == UINT32_MAX);
    CHECK_FALSE(system->_test_atlas_allocator().get_slot(chunk_key, mapped_slot));
}

TEST_CASE("[Streaming Pipeline][UploadPipeline] cancel_asset_jobs only clears targeted asset pending work") {
    const UploadPipelineCancellationDeviceHandle rd_handle = _acquire_upload_pipeline_cancellation_device();
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

    const uint32_t asset_a = 6200;
    const uint32_t asset_b = 6201;
    system->register_asset(asset_a, _create_upload_pipeline_cancellation_test_data());
    system->register_asset(asset_b, _create_upload_pipeline_cancellation_test_data());

    GaussianStreamingSystem &system_ref = *system.ptr();
    auto &uploads = system->_internal_get_upload_pipeline();
    uploads.stop_pack_threads(system_ref);
    uploads.async_pack_enabled = true;

    REQUIRE(uploads.queue_chunk_load(system_ref, asset_a, 0));
    REQUIRE(uploads.queue_chunk_load(system_ref, asset_b, 0));
    REQUIRE(system->get_pending_pack_jobs() == 2);
    REQUIRE(uploads.pack_jobs_in_flight.load(std::memory_order_acquire) == 2);

    auto *state_a = system->_test_get_asset_state(asset_a);
    auto *state_b = system->_test_get_asset_state(asset_b);
    REQUIRE(state_a != nullptr);
    REQUIRE(state_b != nullptr);

    auto &chunks_a = system->_test_get_asset_chunks(*state_a);
    auto &chunks_b = system->_test_get_asset_chunks(*state_b);
    REQUIRE(chunks_a.size() > 0);
    REQUIRE(chunks_b.size() > 0);

    auto &chunk_a = chunks_a[0];
    auto &chunk_b = chunks_b[0];
    REQUIRE(chunk_a.upload_pending);
    REQUIRE(chunk_b.upload_pending);
    REQUIRE(chunk_a.buffer_slot != UINT32_MAX);
    REQUIRE(chunk_b.buffer_slot != UINT32_MAX);

    const uint64_t chunk_key_a = system->_test_make_chunk_key(asset_a, 0);
    const uint64_t chunk_key_b = system->_test_make_chunk_key(asset_b, 0);
    uint32_t mapped_slot_a = UINT32_MAX;
    uint32_t mapped_slot_b = UINT32_MAX;
    REQUIRE(system->_test_atlas_allocator().get_slot(chunk_key_a, mapped_slot_a));
    REQUIRE(system->_test_atlas_allocator().get_slot(chunk_key_b, mapped_slot_b));
    REQUIRE(mapped_slot_a == chunk_a.buffer_slot);
    REQUIRE(mapped_slot_b == chunk_b.buffer_slot);

    uploads.cancel_asset_jobs(system_ref, asset_a);

    CHECK(system->get_pending_pack_jobs() == 1);
    CHECK(system->get_pending_upload_jobs() == 0);
    CHECK(uploads.pack_jobs_in_flight.load(std::memory_order_acquire) == 1);
    CHECK_FALSE(chunk_a.upload_pending);
    CHECK_FALSE(chunk_a.is_loaded);
    CHECK(chunk_a.buffer_slot == UINT32_MAX);
    CHECK_FALSE(system->_test_atlas_allocator().get_slot(chunk_key_a, mapped_slot_a));

    CHECK(chunk_b.upload_pending);
    CHECK(chunk_b.buffer_slot != UINT32_MAX);
    CHECK(system->_test_atlas_allocator().get_slot(chunk_key_b, mapped_slot_b));
    CHECK(mapped_slot_b == chunk_b.buffer_slot);

    uploads.cancel_asset_jobs(system_ref, asset_b);
    CHECK(system->get_pending_pack_jobs() == 0);
    CHECK(system->get_pending_upload_jobs() == 0);
    CHECK(uploads.pack_jobs_in_flight.load(std::memory_order_acquire) == 0);
    CHECK_FALSE(chunk_b.upload_pending);
    CHECK(chunk_b.buffer_slot == UINT32_MAX);
    CHECK_FALSE(system->_test_atlas_allocator().get_slot(chunk_key_b, mapped_slot_b));
}

} // namespace TestGaussianSplatting

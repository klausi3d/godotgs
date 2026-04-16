#pragma once

#define _ALLOW_KEYWORD_MACROS
#define private public
#include "../core/gaussian_streaming.h"
#undef private

#include "test_macros.h"
#include "servers/rendering_server.h"

namespace {

Ref<GaussianData> _gs_upload_cancel_create_test_data(uint32_t p_count = 1024) {
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

struct GSUploadCancelTestRenderingDeviceHandle {
    RenderingDevice *rd = nullptr;
    bool owns_rd = false;

    ~GSUploadCancelTestRenderingDeviceHandle() {
        if (owns_rd && rd) {
            memdelete(rd);
        }
    }
};

GSUploadCancelTestRenderingDeviceHandle _gs_upload_cancel_get_test_rendering_device() {
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

TEST_CASE("[Streaming Pipeline] cancel_chunk_jobs ignores non-matching buffer slots") {
    const GSUploadCancelTestRenderingDeviceHandle rd_handle = _gs_upload_cancel_get_test_rendering_device();
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

    const uint32_t asset_id = 5001;
    const uint32_t splat_count = GaussianStreamingSystem::CHUNK_SIZE + 128;
    system->register_asset(asset_id, _gs_upload_cancel_create_test_data(splat_count));

    StreamingUploadPipeline &uploads = system->_internal_get_upload_pipeline();
    uploads.stop_pack_threads(*system.ptr());
    uploads.async_pack_enabled = true;

    REQUIRE(uploads.queue_chunk_load(*system.ptr(), asset_id, 0));
    REQUIRE(uploads.get_pack_queue_depth_cached() == 1);

    GaussianStreamingSystem::AtlasAssetState *asset = system->_get_asset_state(asset_id);
    REQUIRE(asset != nullptr);
    auto &chunks = system->_get_asset_chunks(*asset);
    REQUIRE(chunks.size() >= 2);

    const uint32_t expected_slot = chunks[0].buffer_slot;
    REQUIRE(chunks[0].upload_pending);
    REQUIRE(expected_slot != UINT32_MAX);

    const uint64_t chunk_key = system->_make_chunk_key(asset_id, 0);
    uint32_t mapped_slot = UINT32_MAX;
    REQUIRE(system->atlas_allocator.get_slot(chunk_key, mapped_slot));
    REQUIRE(mapped_slot == expected_slot);

    const uint32_t wrong_slot = (expected_slot == 0) ? 1u : (expected_slot - 1u);
    uploads.cancel_chunk_jobs(*system.ptr(), asset_id, 0, wrong_slot);

    CHECK(chunks[0].upload_pending);
    CHECK_FALSE(chunks[0].is_loaded);
    CHECK(chunks[0].buffer_slot == expected_slot);
    CHECK(uploads.get_pack_queue_depth_cached() == 1);
    CHECK(uploads.get_upload_queue_depth_cached() == 0);

    mapped_slot = UINT32_MAX;
    CHECK(system->atlas_allocator.get_slot(chunk_key, mapped_slot));
    CHECK(mapped_slot == expected_slot);
}

TEST_CASE("[Streaming Pipeline] cancel_chunk_jobs removes only the targeted chunk") {
    const GSUploadCancelTestRenderingDeviceHandle rd_handle = _gs_upload_cancel_get_test_rendering_device();
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

    const uint32_t asset_id = 5002;
    const uint32_t splat_count = GaussianStreamingSystem::CHUNK_SIZE + 128;
    system->register_asset(asset_id, _gs_upload_cancel_create_test_data(splat_count));

    StreamingUploadPipeline &uploads = system->_internal_get_upload_pipeline();
    uploads.stop_pack_threads(*system.ptr());
    uploads.async_pack_enabled = true;

    REQUIRE(uploads.queue_chunk_load(*system.ptr(), asset_id, 0));
    REQUIRE(uploads.queue_chunk_load(*system.ptr(), asset_id, 1));
    CHECK(uploads.get_pack_queue_depth_cached() == 2);

    GaussianStreamingSystem::AtlasAssetState *asset = system->_get_asset_state(asset_id);
    REQUIRE(asset != nullptr);
    auto &chunks = system->_get_asset_chunks(*asset);
    REQUIRE(chunks.size() >= 2);

    const uint32_t slot_chunk_0 = chunks[0].buffer_slot;
    const uint32_t slot_chunk_1 = chunks[1].buffer_slot;
    REQUIRE(chunks[0].upload_pending);
    REQUIRE(chunks[1].upload_pending);
    REQUIRE(slot_chunk_0 != UINT32_MAX);
    REQUIRE(slot_chunk_1 != UINT32_MAX);

    const uint64_t chunk_key_0 = system->_make_chunk_key(asset_id, 0);
    const uint64_t chunk_key_1 = system->_make_chunk_key(asset_id, 1);

    uploads.cancel_chunk_jobs(*system.ptr(), asset_id, 0, slot_chunk_0);

    CHECK_FALSE(chunks[0].upload_pending);
    CHECK_FALSE(chunks[0].is_loaded);
    CHECK(chunks[0].buffer_slot == UINT32_MAX);
    CHECK_FALSE(chunks[1].is_loaded);
    CHECK(chunks[1].upload_pending);
    CHECK(chunks[1].buffer_slot == slot_chunk_1);

    uint32_t mapped_slot = UINT32_MAX;
    CHECK_FALSE(system->atlas_allocator.get_slot(chunk_key_0, mapped_slot));
    CHECK(system->atlas_allocator.get_slot(chunk_key_1, mapped_slot));
    CHECK(mapped_slot == slot_chunk_1);
    CHECK(uploads.get_pack_queue_depth_cached() == 1);
    CHECK(uploads.get_upload_queue_depth_cached() == 0);
    CHECK(uploads.pack_jobs_in_flight.load(std::memory_order_acquire) == 1);
}

TEST_CASE("[Streaming Pipeline] cancel_asset_jobs clears mixed pack and upload queues") {
    const GSUploadCancelTestRenderingDeviceHandle rd_handle = _gs_upload_cancel_get_test_rendering_device();
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

    const uint32_t asset_id = 5003;
    const uint32_t splat_count = GaussianStreamingSystem::CHUNK_SIZE + 128;
    system->register_asset(asset_id, _gs_upload_cancel_create_test_data(splat_count));

    StreamingUploadPipeline &uploads = system->_internal_get_upload_pipeline();
    uploads.stop_pack_threads(*system.ptr());
    uploads.async_pack_enabled = true;

    REQUIRE(uploads.queue_chunk_load(*system.ptr(), asset_id, 0));
    REQUIRE(uploads.queue_chunk_load(*system.ptr(), asset_id, 1));
    REQUIRE(uploads.get_pack_queue_depth_cached() == 2);
    REQUIRE(uploads.pack_jobs_in_flight.load(std::memory_order_acquire) == 2);
    REQUIRE(uploads.promote_pack_jobs_sync(1) == 1);
    REQUIRE(uploads.get_pack_queue_depth_cached() == 1);
    REQUIRE(uploads.get_upload_queue_depth_cached() == 1);
    REQUIRE(uploads.pack_jobs_in_flight.load(std::memory_order_acquire) == 1);

    GaussianStreamingSystem::AtlasAssetState *asset = system->_get_asset_state(asset_id);
    REQUIRE(asset != nullptr);
    auto &chunks = system->_get_asset_chunks(*asset);
    REQUIRE(chunks.size() >= 2);

    uploads.cancel_asset_jobs(*system.ptr(), asset_id);

    CHECK(uploads.get_pack_queue_depth_cached() == 0);
    CHECK(uploads.get_upload_queue_depth_cached() == 0);
    CHECK(uploads.pack_jobs_in_flight.load(std::memory_order_acquire) == 0);

    const uint64_t chunk_key_0 = system->_make_chunk_key(asset_id, 0);
    const uint64_t chunk_key_1 = system->_make_chunk_key(asset_id, 1);
    uint32_t mapped_slot = UINT32_MAX;
    CHECK_FALSE(system->atlas_allocator.get_slot(chunk_key_0, mapped_slot));
    CHECK_FALSE(system->atlas_allocator.get_slot(chunk_key_1, mapped_slot));
    CHECK_FALSE(chunks[0].upload_pending);
    CHECK_FALSE(chunks[0].is_loaded);
    CHECK(chunks[0].buffer_slot == UINT32_MAX);
    CHECK_FALSE(chunks[1].upload_pending);
    CHECK_FALSE(chunks[1].is_loaded);
    CHECK(chunks[1].buffer_slot == UINT32_MAX);
}

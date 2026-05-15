/**************************************************************************/
/*  gs_gpu_test_spike.cpp                                                 */
/*  Phase 1 feasibility spike for the Gaussian Splatting GPU test harness.*/
/*  Brings up an offscreen RenderingDevice under Main::test_setup() and   */
/*  proves RenderingDevice::get_singleton() returns our instance + a      */
/*  trivial texture round-trip works. No doctest, no module test code,    */
/*  no RenderingServer init. Exits 0 on success, non-zero per code below. */
/**************************************************************************/

#ifdef TESTS_ENABLED

#include "core/error/error_macros.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/string/ustring.h"
#include "main/main.h"

#if defined(RD_ENABLED)
#include "servers/rendering/rendering_device.h"
#if defined(VULKAN_ENABLED)
#include "drivers/vulkan/rendering_context_driver_vulkan.h"
#endif
#ifdef D3D12_ENABLED
#include "drivers/d3d12/rendering_context_driver_d3d12.h"
#endif
#endif

namespace {

// Exit codes — kept aligned with the future full harness contract.
constexpr int SPIKE_OK = 0;
constexpr int SPIKE_FAIL_TEST_SETUP = 70;
constexpr int SPIKE_FAIL_NO_DRIVER = 64;
constexpr int SPIKE_FAIL_DRIVER_INIT = 65;
constexpr int SPIKE_FAIL_RD_INIT = 66;
constexpr int SPIKE_FAIL_SINGLETON_MISMATCH = 67;
constexpr int SPIKE_FAIL_TEXTURE_CREATE = 68;
constexpr int SPIKE_FAIL_FORMAT_READBACK = 69;
constexpr int SPIKE_FAIL_RD_NOT_COMPILED = 71;

#if defined(RD_ENABLED)
int _spike_run() {
    print_line("[GS-GPU-SPIKE] starting offscreen RD bootstrap");

    RenderingContextDriver *rcd = nullptr;
    const char *driver_label = nullptr;

#if defined(VULKAN_ENABLED)
    rcd = memnew(RenderingContextDriverVulkan);
    driver_label = "vulkan";
#endif
#ifdef D3D12_ENABLED
    if (rcd == nullptr) {
        rcd = memnew(RenderingContextDriverD3D12);
        driver_label = "d3d12";
    }
#endif

    if (rcd == nullptr) {
        print_error("[GS-GPU-SPIKE] FAIL: no rendering context driver compiled in");
        return SPIKE_FAIL_NO_DRIVER;
    }
    print_line(vformat("[GS-GPU-SPIKE] driver=%s", driver_label));

    Error err = rcd->initialize();
    if (err != OK) {
        print_error(vformat("[GS-GPU-SPIKE] FAIL: rcd->initialize() returned %d", int(err)));
        memdelete(rcd);
        return SPIKE_FAIL_DRIVER_INIT;
    }

    RenderingDevice *rd = memnew(RenderingDevice);
    err = rd->initialize(rcd);
    if (err != OK) {
        print_error(vformat("[GS-GPU-SPIKE] FAIL: rd->initialize() returned %d", int(err)));
        memdelete(rd);
        memdelete(rcd);
        return SPIKE_FAIL_RD_INIT;
    }

    if (RenderingDevice::get_singleton() != rd) {
        print_error("[GS-GPU-SPIKE] FAIL: RenderingDevice::get_singleton() did not return our instance");
        memdelete(rd);
        memdelete(rcd);
        return SPIKE_FAIL_SINGLETON_MISMATCH;
    }
    print_line("[GS-GPU-SPIKE] RenderingDevice::get_singleton() returns our RD");

    RD::TextureFormat tf;
    tf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
    tf.width = 64;
    tf.height = 64;
    tf.depth = 1;
    tf.array_layers = 1;
    tf.mipmaps = 1;
    tf.texture_type = RD::TEXTURE_TYPE_2D;
    tf.samples = RD::TEXTURE_SAMPLES_1;
    tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;

    RID tex = rd->texture_create(tf, RD::TextureView());
    if (!tex.is_valid()) {
        print_error("[GS-GPU-SPIKE] FAIL: texture_create returned invalid RID");
        memdelete(rd);
        memdelete(rcd);
        return SPIKE_FAIL_TEXTURE_CREATE;
    }

    const RD::TextureFormat got = rd->texture_get_format(tex);
    if (got.width != 64 || got.height != 64 || got.format != RD::DATA_FORMAT_R8G8B8A8_UNORM) {
        print_error(vformat(
                "[GS-GPU-SPIKE] FAIL: texture_get_format returned w=%d h=%d fmt=%d, expected 64x64 R8G8B8A8_UNORM",
                int(got.width), int(got.height), int(got.format)));
        rd->free(tex);
        memdelete(rd);
        memdelete(rcd);
        return SPIKE_FAIL_FORMAT_READBACK;
    }
    print_line(vformat("[GS-GPU-SPIKE] texture_create + format readback OK (%dx%d)",
            int(got.width), int(got.height)));

    rd->free(tex);

    const String adapter_name = rd->get_device_name();
    const String adapter_vendor = rd->get_device_vendor_name();
    print_line(vformat("[GS-GPU-SPIKE] adapter=\"%s\" vendor=\"%s\"", adapter_name, adapter_vendor));

    memdelete(rd);
    memdelete(rcd);

    print_line(vformat("SPIKE_OK driver=%s rd_singleton_set=1 texture_roundtrip=1 adapter=\"%s\"",
            driver_label, adapter_name));
    return SPIKE_OK;
}
#endif // RD_ENABLED

} // namespace

int gs_gpu_test_spike_main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

#if !defined(RD_ENABLED)
    print_error("[GS-GPU-SPIKE] FAIL: build is missing RD_ENABLED; no RenderingDevice support compiled");
    return SPIKE_FAIL_RD_NOT_COMPILED;
#else
    Error setup_err = Main::test_setup();
    if (setup_err != OK) {
        print_error(vformat("[GS-GPU-SPIKE] FAIL: Main::test_setup() returned %d", int(setup_err)));
        return SPIKE_FAIL_TEST_SETUP;
    }

    int rc = _spike_run();

    Main::test_cleanup();
    return rc;
#endif
}

#endif // TESTS_ENABLED

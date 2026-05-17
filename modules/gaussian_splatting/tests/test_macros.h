/**************************************************************************/
/*  test_macros.h                                                         */
/*  Gaussian Splatting Module Test Framework                              */
/**************************************************************************/

#ifndef GAUSSIAN_SPLATTING_TEST_MACROS_H
#define GAUSSIAN_SPLATTING_TEST_MACROS_H

// Include Godot's test framework - doctest is built into Godot
#include "tests/test_macros.h"

// Additional test utilities specific to Gaussian Splatting
#include "core/os/os.h"
#include "servers/rendering_server.h"
#include "servers/rendering/rendering_device.h"

// Performance testing utilities
class PerformanceTimer {
private:
    uint64_t start_time;
    String name;

public:
    PerformanceTimer(const String &p_name) : name(p_name) {
        start_time = OS::get_singleton()->get_ticks_usec();
    }

    float elapsed_ms() const {
        uint64_t elapsed = OS::get_singleton()->get_ticks_usec() - start_time;
        return elapsed / 1000.0f;
    }

    void print_elapsed() const {
        print_line(vformat("[%s] Elapsed: %.2f ms", name, elapsed_ms()));
    }
};

// Simple GPU memory tracker for tests
// NOTE: Named TestGPUMemoryTracker to avoid conflict with memory_validator.h's GPUMemoryTracker
class TestGPUMemoryTracker {
private:
    RenderingDevice *rd;
    uint64_t initial_memory;

public:
    TestGPUMemoryTracker(RenderingDevice *p_rd) : rd(p_rd), initial_memory(0) {
        if (rd) {
            initial_memory = rd->get_memory_usage(RenderingDevice::MEMORY_TOTAL);
        }
    }

    float get_memory_usage_mb() const {
        if (!rd) return 0.0f;
        uint64_t current = rd->get_memory_usage(RenderingDevice::MEMORY_TOTAL);
        return (current - initial_memory) / (1024.0f * 1024.0f);
    }

    void print_usage() const {
        print_line(vformat("GPU Memory Usage: %.2f MB", get_memory_usage_mb()));
    }
};

// RAII guard for the optional locally-allocated fallback RenderingDevice that
// REQUIRE_GPU_DEVICE() may construct when RenderingDevice::get_singleton()
// returns null. Frees the device on scope exit if-and-only-if the macro owned
// it. `rd` is exposed as a public field so the macro can republish it under
// that name to the surrounding test function — the call-site contract from
// pre-refactor REQUIRE_GPU_DEVICE() is unchanged.
class ScopedFallbackRD {
public:
    RenderingDevice *rd = nullptr;
    bool owns_rd = false;

    ScopedFallbackRD() {
        rd = RenderingDevice::get_singleton();
        if (!rd) {
            if (RenderingServer *rs = RenderingServer::get_singleton()) {
                rd = rs->create_local_rendering_device();
                owns_rd = (rd != nullptr);
            }
        }
    }
    ~ScopedFallbackRD() {
        if (owns_rd && rd) {
            memdelete(rd);
        }
    }
    ScopedFallbackRD(const ScopedFallbackRD &) = delete;
    ScopedFallbackRD &operator=(const ScopedFallbackRD &) = delete;
};

// Helper macro for GPU tests that require a rendering device.
// Publishes a `RenderingDevice *rd` into the calling test's scope. If the
// engine singleton is null, a local fallback RD is created and OWNED by an
// inline ScopedFallbackRD guard — the guard's destructor frees the device on
// every scope exit (normal return, early return below, doctest REQUIRE
// exception). Pre-refactor callers leaked the fallback RD because the macro
// had no paired teardown; #334.
#define REQUIRE_GPU_DEVICE()                                              \
    ScopedFallbackRD _gs_rd_scope;                                        \
    RenderingDevice *rd = _gs_rd_scope.rd;                                \
    if (rd == nullptr) {                                                  \
        MESSAGE("Skipping test - RenderingDevice unavailable");           \
        return;                                                           \
    }

// Performance baseline checking
#define CHECK_PERFORMANCE(timer, max_ms, operation)                      \
    CHECK_MESSAGE(timer.elapsed_ms() < max_ms,                          \
        vformat(operation " took %.2fms, expected < %.2fms",            \
            timer.elapsed_ms(), max_ms))

#endif // GAUSSIAN_SPLATTING_TEST_MACROS_H

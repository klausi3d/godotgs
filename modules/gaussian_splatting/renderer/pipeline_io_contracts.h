#ifndef GS_PIPELINE_IO_CONTRACTS_H
#define GS_PIPELINE_IO_CONTRACTS_H

#include <cstddef>
#include <cstdint>

namespace GaussianSplatting {

// Indirect dispatch buffer layout shared between CPU and GPU paths.
// Offsets are documented for shader/CPU alignment.
struct IndirectDispatchLayout {
    uint32_t dispatch_x;      // offset 0
    uint32_t dispatch_y;      // offset 4
    uint32_t dispatch_z;      // offset 8
    uint32_t element_count;   // offset 12
    uint32_t overflow_flag;   // offset 16
    uint32_t unclamped_total; // offset 20
};

static_assert(offsetof(IndirectDispatchLayout, dispatch_x) == 0, "dispatch_x offset mismatch");
static_assert(offsetof(IndirectDispatchLayout, dispatch_y) == 4, "dispatch_y offset mismatch");
static_assert(offsetof(IndirectDispatchLayout, dispatch_z) == 8, "dispatch_z offset mismatch");
static_assert(offsetof(IndirectDispatchLayout, element_count) == 12, "element_count offset mismatch");
static_assert(offsetof(IndirectDispatchLayout, overflow_flag) == 16, "overflow_flag offset mismatch");
static_assert(offsetof(IndirectDispatchLayout, unclamped_total) == 20, "unclamped_total offset mismatch");
static_assert(sizeof(IndirectDispatchLayout) == sizeof(uint32_t) * 6, "IndirectDispatchLayout size mismatch");

static constexpr size_t kIndirectDispatchElementCountOffset = offsetof(IndirectDispatchLayout, element_count);
static constexpr size_t kIndirectDispatchHeaderSize = offsetof(IndirectDispatchLayout, overflow_flag);
static constexpr size_t kIndirectDispatchElementCountReadbackSize =
        sizeof(IndirectDispatchLayout) - kIndirectDispatchElementCountOffset;

} // namespace GaussianSplatting

#endif // GS_PIPELINE_IO_CONTRACTS_H

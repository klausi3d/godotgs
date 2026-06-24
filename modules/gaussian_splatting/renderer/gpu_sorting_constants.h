#ifndef GPU_SORTING_CONSTANTS_H
#define GPU_SORTING_CONSTANTS_H

#include <cstdint>

namespace GPUSortingConstants {

static constexpr uint32_t DEFAULT_WORKGROUP_SIZE = 256;
// Radix-sort digit width PER PASS (4-bit = 16 passes for a 64-bit key, 8-bit = 8 passes). This is a
// performance/mechanics knob ONLY — it does NOT change the sorted output or key precision (precision is
// key_bits/tile_bits/depth_bits). Both 4 and 8 are valid (validate() accepts both) and both are now
// CORRECT at any workgroup_size (the strided per-bin shader loops in gpu_sorter.cpp + the shared-memory
// probe in RadixSort::is_supported handle 8-bit's 256 bins). DEFAULT IS 4-BIT: measured A/B on
// GrandmasHouse (2026-06-06) showed 8-bit is SLOWER, not faster — fewer passes are outweighed by 16x
// larger histograms, more per-pass bin work, and lower occupancy (~10 KB shared mem). Keep 4-bit unless
// a future measurement on different hardware/workload shows otherwise.
static constexpr uint32_t DEFAULT_RADIX_BITS = 4;
static constexpr uint32_t RADIX_BITS = 8;
static constexpr uint32_t RADIX_SIZE = 1u << RADIX_BITS;
static constexpr uint32_t MAX_WORKGROUP_SIZE = 1024;
static constexpr uint32_t HISTOGRAM_BINS = RADIX_SIZE;

} // namespace GPUSortingConstants

#endif // GPU_SORTING_CONSTANTS_H

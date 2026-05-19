#ifndef STREAMING_CHUNK_BAKE_H
#define STREAMING_CHUNK_BAKE_H

#include "core/math/aabb.h"
#include "core/math/vector3.h"
#include "core/object/ref_counted.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"
#include "core/variant/variant.h"

class GaussianSplatAsset;
class GaussianData;

// Per-chunk spatial bookkeeping baked at import time so runtime
// _build_chunks_for_data() can skip the per-splat center / bounds pass on
// scene load. Layout is fixed and POD; serialized as a raw byte blob.
struct StreamingChunkBakeRecord {
    uint32_t start_idx = 0;
    uint32_t count = 0;
    Vector3 center;
    float max_radius = 0.0f;
    AABB bounds;
};

// 4 (start_idx) + 4 (count) + 12 (center) + 4 (max_radius) + 24 (bounds) = 48 bytes in
// single-precision (real_t = float) builds. Double-precision builds widen Vector3/AABB
// and naturally bump this number; the bake format is a per-build runtime cache so
// drift across precision builds is acceptable, but drift within a precision flavor
// would silently corrupt loads.
#ifndef REAL_T_IS_DOUBLE
static_assert(sizeof(StreamingChunkBakeRecord) == 48,
        "StreamingChunkBakeRecord layout drifted; bake on-disk format would silently break.");
#endif

// Quantization params snapshot captured at bake time and re-checked at
// runtime to decide whether the baked quantization can be reused or must be
// recomputed from gaussians. Kept POD for trivial serialization.
struct ChunkQuantizationRuntimeParams {
    uint32_t position_bits = 16;
    uint32_t scale_bits = 12;
    bool scales_quantized = false;
    bool enabled = false;
};

namespace StreamingChunkBakeIO {

PackedByteArray serialize_records(const Vector<StreamingChunkBakeRecord> &p_records);
bool deserialize_records(const PackedByteArray &p_bytes, Vector<StreamingChunkBakeRecord> &r_records);

}  // namespace StreamingChunkBakeIO

// Walk every Gaussian once, compute per-chunk center / max_radius / bounds,
// and stash the results on the asset so runtime registration can skip the
// O(N) pass. p_include_primary controls whether to also bake the Morton-sorted
// primary source-index remap; for per-asset registration paths this is false
// (see GaussianStreamingSystem::_build_chunks_for_data; build_primary_spatial
// is false for non-primary assets).
void bake_streaming_chunks_for_asset(
        Ref<GaussianSplatAsset> p_asset,
        const Ref<GaussianData> &p_data,
        bool p_include_primary_morton,
        const ChunkQuantizationRuntimeParams *p_quant_params);

#endif  // STREAMING_CHUNK_BAKE_H

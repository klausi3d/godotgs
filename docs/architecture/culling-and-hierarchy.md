# Culling and Hierarchy

This page documents the **truthful** state of coarse culling and spatial hierarchy in the Gaussian
Splatting module. It exists because several project settings, code symbols, and earlier docs imply
that a cluster-culling stage is part of the live render path. As of 2026-04-26 it is not.

Read this page before assuming any cluster-culling project setting reaches a live code branch.

## Current active coarse culling path

The live coarse culling path is the LOD-driven octree query inside `GPUCuller`.

- Entry point: [../../modules/gaussian_splatting/interfaces/gpu_culler.cpp](../../modules/gaussian_splatting/interfaces/gpu_culler.cpp)
  — `GPUCuller::cull_for_view()` and the helper `GPUCuller::ensure_hierarchical_structure()`.
- Hierarchy implementation: [../../modules/gaussian_splatting/lod/hierarchical_splat_structure.h](../../modules/gaussian_splatting/lod/hierarchical_splat_structure.h)
  / [.cpp](../../modules/gaussian_splatting/lod/hierarchical_splat_structure.cpp) —
  `GaussianSplatting::HierarchicalSplatStructure`, an octree built from the loaded
  `GaussianData` storage and queried with `query_visible_splats(frustum, camera, lod_bias, max_query)`.
- Owning state: `GPUCuller::CullingState::hierarchical_structure` in [../../modules/gaussian_splatting/interfaces/gpu_culler.h](../../modules/gaussian_splatting/interfaces/gpu_culler.h).
- LOD config feeding the query: [../../modules/gaussian_splatting/lod/lod_config.h](../../modules/gaussian_splatting/lod/lod_config.h)
  / [.cpp](../../modules/gaussian_splatting/lod/lod_config.cpp), with `LOD_CONFIG_*` settings under
  `rendering/gaussian_splatting/lod/`.

### Inputs

- `GaussianData` storage (positions, scales, opacity, importance) from the loaded splat asset.
- Camera position, frustum planes, and viewport extracted by `GPUCuller` from the active view.
- `culling_state.culling_octree_max_depth` (default `8`) and
  `culling_state.culling_min_gaussians` (default `32`).
- `culling_config.lod_bias` (effective bias from `lod_config.h`).

### Outputs

- `QueryResult { visible_indices, lod_weights }` from the octree query, fed into
  `culling_state.culled_indices` / `culled_importance_weights`.
- Down the pipe these flow into the GPU sort and tile raster stages described in
  [render-pipeline.md](render-pipeline.md).

### Renderer integration

The active call chain is:

`GaussianSplatRenderer::render_scene_instance` -> `RenderPipelineStages::execute_cull_stage` ->
`GPUCuller::cull_for_view` -> `ensure_hierarchical_structure` ->
`HierarchicalSplatStructure::query_visible_splats` -> sort/raster stages.

The cluster-cull stack below is **not** part of this chain.

## Dormant experimental / legacy cluster stack

The following code is registered and compiles, but is never instantiated or invoked from the
production render path. It is dead weight on the surface: do not assume project settings flow
through it.

- [../../modules/gaussian_splatting/interfaces/cluster_culler.h](../../modules/gaussian_splatting/interfaces/cluster_culler.h)
  / [.cpp](../../modules/gaussian_splatting/interfaces/cluster_culler.cpp) — `ClusterCuller`
  resource. Reads `rendering/gaussian_splatting/culling/cluster_culling_enabled` and
  `cluster_target_size` (see [../../modules/gaussian_splatting/interfaces/cluster_culler.cpp](../../modules/gaussian_splatting/interfaces/cluster_culler.cpp),
  `:104-108`). The third related key, `cluster_frustum_slack`, is registered in
  [../../modules/gaussian_splatting/core/gaussian_splat_manager.cpp](../../modules/gaussian_splatting/core/gaussian_splat_manager.cpp)
  (`:1035`) and consumed only as pipeline-state hash input in
  [../../modules/gaussian_splatting/renderer/render_pipeline_stages.cpp](../../modules/gaussian_splatting/renderer/render_pipeline_stages.cpp)
  (`:587`); `ClusterCuller` itself does not read it. No production call site constructs
  `ClusterCuller`.
- [../../modules/gaussian_splatting/lod/cluster_builder.h](../../modules/gaussian_splatting/lod/cluster_builder.h)
  / [.cpp](../../modules/gaussian_splatting/lod/cluster_builder.cpp) — CPU Morton clustering helper.
  Only referenced by `cluster_culler.{h,cpp}`; not used by `GPUCuller` or any pipeline stage.
- [../../modules/gaussian_splatting/compute/cluster_cull.glsl](../../modules/gaussian_splatting/compute/cluster_cull.glsl)
  (and its build-time-generated `cluster_cull.glsl.gen.h` header — produced by SCons,
  not source-controlled) — coarse-pass GPU shader. Loaded only by the dormant
  `ClusterCuller`; not bound by `RenderPipelineStages` or `GPUSortingPipeline`.
- [../../modules/gaussian_splatting/interfaces/gpu_culler.h](../../modules/gaussian_splatting/interfaces/gpu_culler.h)
  (`:318`) — `Ref<ClusterCuller> cluster_culler;` field on `GPUCuller`. **Never assigned anywhere
  in the module** (verified 2026-04-26 via full-tree grep for `cluster_culler =`,
  `cluster_culler.instantiate`, `memnew(ClusterCuller`, `new ClusterCuller`).
- [../../modules/gaussian_splatting/renderer/pipeline_io_contracts.h](../../modules/gaussian_splatting/renderer/pipeline_io_contracts.h)
  (`:34-49`) — `ClusterCullIndirectDispatchLayout` struct and its `static_assert` size checks. The
  layout exists for the dormant shader; no production producer or consumer fills it.

The class is registered at [../../modules/gaussian_splatting/register_types.cpp](../../modules/gaussian_splatting/register_types.cpp)
(`:142`, `GDREGISTER_CLASS(ClusterCuller);`) but never instantiated in production C++. Do not
assume project settings reach this path.

The `culling_config.cluster_culling_enabled`, `cluster_target_size`, `cluster_frustum_slack`,
`cluster_use_morton_order`, and `cluster_use_indirect_dispatch` fields on
`GPUCuller::CullingConfig` (see [../../modules/gaussian_splatting/interfaces/gpu_culler.h](../../modules/gaussian_splatting/interfaces/gpu_culler.h),
`:51-57`) are only consumed by pipeline-state hashing in
[../../modules/gaussian_splatting/renderer/render_pipeline_stages.cpp](../../modules/gaussian_splatting/renderer/render_pipeline_stages.cpp)
(`:585-589`). They influence cache keys but do not gate any live cull work.

### Note on `GaussianData::octree`

`GaussianData` carries its own octree (see [../../modules/gaussian_splatting/core/gaussian_data.h](../../modules/gaussian_splatting/core/gaussian_data.h),
`build_octree(...)` / `query_octree(AABB)`). This is a **separate query utility** for asset-side
spatial lookups (asset queries, debug tooling, importer paths). It is **not** the active culler
and not the same structure as `HierarchicalSplatStructure`. Do not conflate the two.

## Future tier-2 cluster-culling rewrite, if pursued

If cluster culling is revived, the design target is the tier-2 spec, not the dormant code above:

- Spec: [tier2_cluster_culling_spec.md](tier2_cluster_culling_spec.md).
- Landed in PR
  [#246 — docs: design specs for cluster culling and resolve-mode lighting](https://github.com/klausi3D/godotGS/pull/246)
  (merge commit `0e38870268e7f108065a4f381a60ff4fabf06c1d`).

The spec is explicit (Section 2, Section 3) that the existing `ClusterCuller` / `ClusterBuilder`
operate on whole-scene `GaussianData` and pack legacy AABB data, not tier-2 chunk-local instance
metadata. Any reuse must be a rewrite, not a revival: cluster records become chunk-local, the
data flow runs through `visible_chunk_buffer` and `splat_ref_buffer`, and new shader sources
(`instance_cluster_dispatch.glsl`, `cluster_cull_instance.glsl`,
`cluster_range_dispatch.glsl`, `cluster_depth_compute.glsl`) would replace the legacy shader at
[../../modules/gaussian_splatting/compute/cluster_cull.glsl](../../modules/gaussian_splatting/compute/cluster_cull.glsl).
The current names and project settings may be reused, but the implementation must be redone
from the tier-2 instance contract.

Until that work lands, the live coarse culling path is the LOD/octree path described in section
1 above.

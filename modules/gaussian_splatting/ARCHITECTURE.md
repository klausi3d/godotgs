# Gaussian Splatting Module Architecture

Related docs: [MEMORY_SUBSYSTEM](MEMORY_SUBSYSTEM.md), [READING_ORDER](READING_ORDER.md), [ABBREVIATIONS](ABBREVIATIONS.md), [README](README.md)

## Module Entry Points

| Entry Point | File | Purpose |
|-------------|------|---------|
| `register_types.cpp` | `register_types.cpp` | Registers all classes with Godot |
| `GaussianSplatManager` | `core/gaussian_splat_manager.cpp` | Singleton managing GPU resources and global config |
| `GaussianSplatNode3D` | `nodes/gaussian_splat_node_3d.cpp` | Main scene node for splat rendering |
| `GaussianSplatRenderer` | `renderer/gaussian_splat_renderer.cpp` | Core rendering pipeline (implements `IRenderer`) |

## Subsystem Map

### Core (`core/`)
- `gaussian_data.*` - Splat data storage (positions, colors, SH coefficients)
- `gaussian_streaming.*` - Chunk-based streaming with VRAM budget management (T8 refactor)
- `gaussian_splat_manager.*` - Global singleton, device management, config registry
- `gaussian_splat_scene_director.*` - Multi-instance coordination and registry
- `gaussian_splat_world.*` - World-scale owner for streaming assets

### Renderer (`renderer/`)
- `gaussian_splat_renderer.*` - Main render orchestration (T9 delegating to orchestrators)
- `render_pipeline_stages.*` - Cull -> Sort -> Raster -> Composite stage runner
- `render_*_orchestrator.*` - Focused subsystems for culling, sorting, streaming, output, and diagnostics (T9 long-method cleanup)
- `gpu_sorter.*` - GPU sorting (Bitonic, Radix, OneSweep)
- `gpu_memory_stream.*` - Triple-buffered GPU uploads
- `tile_renderer.*` / `tile_render_resources.*` - Tile-based rasterization; the graphics raster pipeline is now created at module init via `tile_render_resources` rather than lazily on first frame (#345), eliminating the first-frame stall.
- `spirv_disk_cache.*` - Persistent on-disk cache for compiled SPIR-V binaries. Keyed per-device (`compute_key()` accepts the `RenderingDevice` so the per-device subdir is resolved under the same mutex critical section as the file op). Integrated through `shader_compilation_helper.cpp`; wired up in `register_types.cpp` (#343).

### Nodes (`nodes/`)
- `gaussian_splat_node_3d.*` - Primary scene node
- `gaussian_splat_container.*` - Multi-splat container
- `gaussian_splat_world_3d.*` - World-scale streaming entry point

### Interfaces (`interfaces/`)
- `renderer_interfaces.h` - `IRenderer` contract and provider
- `rasterizer_interfaces.h`, `gpu_sorting_pipeline_interfaces.h`, `output_compositor_interfaces.h` - Dependency-inverted rendering components
- `output_compositor.{cpp,h}` - Implements `IOutputCompositor`. Owns the scratch-copy compute path (see Output Compositing below).

### Logger (`logger/`)
- `startup_trace.{cpp,h}` - RAII timer system `GS_STARTUP_SCOPE(name)`. Each `[StartupTrace]` line emits at the start of an asset's first rendered frame, attributing time across ~15 phases (`module_register`, `manager_construct`, `device_request`, `renderer_construct`, `gpu_buffer_manager_init`, `shader_compile_binning/prefix/raster`, `sorter_create_variant`, `streaming_persistent_buffer_alloc`, `streaming_atlas_build_cpu/sync_gpu`, `first_frame_raster_pipeline_create`, `ply_payload_parse`, `asset_populate_gaussian_data`). Thread-safe accumulator with `flush_one_pending()` under mutex and per-asset-open `sealed_traces` snapshots; drained at `execute_frame_entry`. Gated by the `rendering/gaussian_splatting/diagnostics/startup_trace` project setting (default on, registered in `register_types.cpp`). Arming sites are `GaussianSplatNode3D`'s three asset-load entry points (PLY migration, in-place reload, drag-drop) - NOT `load_from_file` or leaf loaders, to avoid arming headless contexts (#342).
- `gs_logger.*`, `gs_debug_trace.*`, `logging_config.h` - Module logging plumbing.

### Memory Subsystem (shared)
- `renderer/gpu_buffer_manager.*` - Resident buffers for non-streaming data
- `renderer/gpu_memory_stream.*` - Triple-buffered uploads + pooling for streaming
- `core/gaussian_streaming.*` - VRAM budget regulation and eviction policy
- See [MEMORY_SUBSYSTEM](MEMORY_SUBSYSTEM.md) for the full budget/config flow

## Data Flow

1. `GaussianSplatNode3D` registers data and instance transforms through `GaussianSplatSceneDirector` (`core/gaussian_splat_scene_director.*`).
2. `GaussianData` and `GaussianSplatAsset` feed the `GaussianStreamingSystem` (`core/gaussian_streaming.*`) for visibility, eviction, and upload decisions.
3. `GaussianSplatRenderer` coordinates GPU residency via `GPUMemoryStream` (`renderer/gpu_memory_stream.*`) and prepares sorting inputs.
4. `RenderPipelineStages` (`renderer/render_pipeline_stages.*`) executes cull -> sort -> raster -> composite, delegating to the orchestrators from T9.
5. `TileRenderer` (`renderer/tile_renderer.*`) writes color/depth targets, and `RenderOutputOrchestrator` (`renderer/render_output_orchestrator.*`) composites into the active viewport or `RenderSceneBuffersRD` via `OutputCompositor`.

## Output Compositing

`interfaces/output_compositor.cpp` owns the final blit/composite into the active render target. The compute path historically did `imageLoad(u_destination_image)` directly on the destination, which produced a workgroup R/W hazard: workgroup N's `imageLoad` could observe stale data from workgroup N-1's `imageStore` of an overlapping tile. This was the root cause of the #256 black-blocks regression.

The fix (PR #332, squash `83634ecfbc`) is a scratch-copy path:

- Before the compute dispatch, `OutputCompositor::copy_to_render_target` copies the destination into a scratch sampled texture.
- The shader (`shaders/viewport_blit.glsl`) reads from `u_destination_scratch` (binding 4) via `texelFetch`, and `u_destination_image` (binding 1) is marked `writeonly` so the destination is never sampled in-place.
- SRGB destinations are canonicalized to UNORM on the scratch side to avoid double sampler decode.
- Scratch textures are LRU-pooled (`VIEWPORT_BLIT_SCRATCH_MAX_PER_KIND = 4`).
- `shareable_formats` is cleared on the scratch format before `texture_create` to avoid rejection when the destination is SRGB-only-shareable.
- The direct-copy and graphics-fallback fallback paths set `OutputCopyResult::depth_test_honored = false` when the caller requested depth testing but the chosen path cannot honor it - so consumers don't get a false-positive `true`.

When modifying compositor code: never sample the destination image bound for writing in the same dispatch. Route reads through the scratch sampled texture.

## State Structs (from T8 Refactor)

The streaming system in `core/gaussian_streaming.h` uses state structs and companion classes for clear separation:
- `VisibilityState` - Chunk culling, camera tracking, LOD blending
- `StreamingEvictionController` - LRU eviction, hysteresis tracking (extracted to `core/streaming_eviction_controller.h`)
- `StreamingUploadPipeline` - Async pack threads, upload bandwidth (extracted to `core/streaming_upload_pipeline.h`)
- `BudgetState` - VRAM regulation, loaded chunk tracking

### Right-sized persistent buffer (PR #344)
`core/gaussian_streaming.cpp` and `core/streaming_atlas.cpp` now size the streaming persistent buffer to actual scene need rather than a fixed upper bound. `streaming_upload_pipeline.cpp` and `render_diagnostics_orchestrator.cpp` surface the right-sized allocation through diagnostics. `_grow_persistent_buffer` preserves prior contents via `buffer_copy` and uses the `atlas_allocator.resize_preserve` path to keep existing allocations valid across growth.

## Shader Compilation

`renderer/shader_compilation_helper.cpp` is the single entry point for compiling module shaders. It consults `SPIRVDiskCache` (`renderer/spirv_disk_cache.{cpp,h}`) keyed by source + defines + device fingerprint. On cache hit the SPIR-V compile step is skipped entirely, which is the dominant cost for module reloads. The cache directory is per-device (resolved under the cache's mutex with the file op, so two threads compiling against different `RenderingDevice`s cannot cross-pollute), with bounded total size via `prune_above()`. Disable via `SPIRVDiskCache::set_enabled(false)` if you need to force fresh compiles while debugging shader changes.

The graphics raster pipeline owned by `tile_render_resources` is also created at module init (PR #345), so the first rendered frame no longer pays the pipeline-create stall. Cached pipeline format is tracked in `tile_render_resources.h` (`tile_raster_pipeline`, plus the framebuffer format the cached pipeline was created against) and invalidated when the framebuffer format changes.

See [READING_ORDER](READING_ORDER.md) for a guided walkthrough and [ABBREVIATIONS](ABBREVIATIONS.md) for naming conventions.

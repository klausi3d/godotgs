# Memory Subsystem Guide

Related docs: [ARCHITECTURE](ARCHITECTURE.md), [READING_ORDER](READING_ORDER.md), [ABBREVIATIONS](ABBREVIATIONS.md), [README](README.md)

This module has two distinct GPU memory paths that share data but serve different runtime modes. The goal is to keep budget logic centralized while allowing each path to manage its own buffers.

## High-Level Layout

```
Resident path (non-streaming)
  GaussianSplatRenderer
    -> GPUBufferManager (double-buffered resident storage)

Streaming path
  GaussianSplatRenderer
    -> GaussianStreamingSystem (visibility + budget)
        -> GaussianMemoryStream (triple-buffered uploads + pool)
```

## Components and Responsibilities

### GPUBufferManager (resident data)
- **Files**: `renderer/gpu_buffer_manager.h`, `renderer/gpu_buffer_manager.cpp`
- **Role**: Allocates and manages resident GPU buffers used when streaming is not active.
- **Buffers**: Double-buffered Gaussian data + sort keys + indices.
- **Memory tracking**: Provides `get_memory_usage_mb()` as a size estimate, but does **not** enforce budgets.

### GaussianMemoryStream (streamed uploads)
- **Files**: `renderer/gpu_memory_stream.h`, `renderer/gpu_memory_stream.cpp`
- **Role**: Triple-buffered upload path with a suballocation pool for streaming chunks.
- **Buffers**: Three GPU buffers + pooled suballocations for reuse.
- **Memory tracking**: Reports allocated/used MB and efficiency; does **not** decide budgets.

### GaussianStreamingSystem + VRAMBudgetRegulator (budgeting)
- **Files**: `core/gaussian_streaming.h`, `core/gaussian_streaming.cpp`
- **Role**: Owns VRAM budget policy and eviction/LOD decisions. This is the **only** place that regulates VRAM budgets.
- **Key structs**: `VRAMBudgetConfig`, `VRAMBudgetRegulator`, `BudgetState`.
- **Persistent buffer sizing**: see [Persistent Buffer Right-Sizing](#persistent-buffer-right-sizing) below.

## Budget Configuration Flow

1. **Defaults** are defined in ProjectSettings via `core/gaussian_splat_manager.cpp`.
2. **Tier presets** apply caps through `QualityTierConfig` and `GaussianSplatNode3D::_apply_quality_tier_limits`.
3. **Per-node overrides** are assembled in `GaussianSplatNode3D::_apply_renderer_settings` and passed into the streaming system via `ConfigOverrides`.
4. **Streaming system** applies overrides to the `VRAMBudgetRegulator` and drives eviction based on usage.

This flow prevents duplication: only the streaming system enforces VRAM budget policy, while buffer managers expose usage stats.

## Persistent Buffer Right-Sizing

The streaming path keeps **one** GPU storage buffer (`persistent_buffer`,
sized once at `GaussianStreamingSystem::initialize()`) that backs every
resident chunk via the atlas allocator. Sizing policy:

1. Start from `asset_chunks = chunks.size()` — the actual number of chunks
   the loaded asset requires, **not** the regulated maximum.
2. Add a 25 percent growth headroom (`MAX(2, asset_chunks / 4)`) so the first
   eviction-pressure event does not immediately force a grow.
3. Clamp to `MAX(initial_capacity, STREAMING_DEFAULT_MIN_CHUNKS_IN_VRAM)` so
   tiny assets still get a usable working set.
4. Cap at `effective_max_chunks` (the budget regulator's current ceiling) so
   the buffer can never exceed the regulated maximum.

The resulting size is recorded as `streaming_initial_capacity` and the buffer
is named `GS_Streaming_PersistentBuffer` for tooling visibility. See
`GaussianStreamingSystem::initialize` in `core/gaussian_streaming.cpp` for the
exact computation.

### Growth Path

When the atlas allocator reports it cannot fit the requested loaded-chunk
count, `GaussianStreamingSystem::_try_grow_persistent_buffer_for_atlas_pressure`
calls `_grow_persistent_buffer(target)`. Growth:

- Allocates a larger storage buffer on the upload device.
- Copies the live region (`persistent_buffer_size` bytes) from the old
  buffer into the new one via `RenderingDevice::buffer_copy`, so currently
  resident chunks remain valid.
- Frees the old buffer, updates `persistent_buffer_size`, and resizes the
  atlas allocator with `resize_preserve` so existing slot indices stay
  stable across the grow.
- Refuses any grow that would exceed `UINT32_MAX` bytes (the
  `RenderingDevice` 32-bit addressing limit) or the regulated chunk ceiling.

Each successful grow increments `streaming_grow_count`.

### Diagnostics Surface

`RenderDiagnosticsOrchestrator` exposes three persistent-buffer metrics
(`renderer/render_diagnostics_orchestrator.cpp`):

| Metric | Meaning |
| --- | --- |
| `streaming_initial_capacity` | Chunks reserved at init (post right-sizing). |
| `streaming_current_capacity` | Chunks the persistent buffer currently fits. |
| `streaming_grow_count` | Number of in-place grows since init. |

A non-zero `streaming_grow_count` for a stable scene is a tuning signal:
either the asset's chunk count was under-estimated at import time, or the
budget regulator is being driven harder than the initial headroom can
absorb. Neither is a correctness bug; the grow path is the supported
mechanism for handling it.

User-facing summary of these settings and metrics:
[Performance Dashboard](../../docs/performance/index.md).

## When to Use Which Path

- **Resident path** (`GPUBufferManager`): small/medium datasets, no streaming, lower per-frame overhead.
- **Streaming path** (`GaussianMemoryStream` + `GaussianStreamingSystem`): large datasets, dynamic loading, budget-aware eviction.

## Debugging and Metrics

- **Budget warnings**: `GaussianStreamingSystem::is_vram_budget_warning_active()`
- **Budget stats**: `GaussianStreamingSystem::get_vram_debug_stats()`
- **Stream usage**: `GaussianMemoryStream::get_allocated_memory_mb()`, `get_used_memory_mb()`, `get_memory_efficiency()`
- **Resident usage**: `GPUBufferManager::get_memory_usage_mb()`

For the per-owner lifetime contract of each buffer in this subsystem (who creates, who destroys, idempotency, threading), see [`docs/architecture/renderer-lifetime-ownership.md`](../../docs/architecture/renderer-lifetime-ownership.md).

## Notes for Future Refactors

- Avoid moving budget logic into `GPUBufferManager` or `GaussianMemoryStream`; the regulator in `core/gaussian_streaming.*` is the single source of truth.
- If a unified memory subsystem is introduced later, preserve the separation between **budget policy** and **buffer allocation**.
- The persistent buffer is **right-sized from the loaded asset**, not from the regulated maximum. Do not regress this to a fixed-cap allocation: large regulated ceilings are common (gigabytes), but most scenes load a fraction of that, and an oversized persistent buffer is pure waste. Growth is wired into the eviction pressure path; trust it.
- Keep `streaming_initial_capacity`, `streaming_current_capacity`, and `streaming_grow_count` flowing through `RenderDiagnosticsOrchestrator`. They are the only signal a user has that the right-sizing heuristic chose well.

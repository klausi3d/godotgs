# Performance Dashboard

This page surfaces the current published benchmark snapshot and the suite lanes that are expected to grow it.

Charts use `assets/data/benchmark_latest.json` generated during docs build.
The current public dataset contains one committed result row, and the coverage table below shows the user-relevant benchmark lanes already defined in the suite.

## Current Public Snapshot

| Lane | Purpose | Score | Avg FPS | P99 Frame (ms) | GPU Time (ms) |
| --- | --- | ---: | ---: | ---: | ---: |
| `static_baseline` | Low-noise raster baseline | 90.7 | 74.0 | 15.62 | 0.0 |

The snapshot above is the current committed public result. It is the reference row used by the charts below until more published scenarios are added.

This row is benchmark evidence, not a release gate. Blocking streaming/runtime readiness is enforced by the runtime validation profile named `streaming-gpu-ci`; open-world benchmark proof surfaces are useful review evidence, but they remain non-blocking unless the workflow contract changes.

## Coverage Map

| Lane | Purpose | Status |
| --- | --- | --- |
| `static_baseline` | Low-noise raster baseline | Published in `benchmark_latest.json` |
| `streaming_corridor` | Camera sweep stressing chunk turnover | Defined in the benchmark suite, not yet published |
| `city_flyover` | High-altitude visibility-change stress | Defined in the benchmark suite, not yet published |
| `instance_storm` | Many-instance submission pressure | Defined in the benchmark suite, not yet published |
| `lighting_stress` | Animated light and shading stress | Defined in the benchmark suite, not yet published |
| `unified_composite` | Integrated all-systems composite lane | Defined in the benchmark suite, not yet published |

Do not cite suite-only or unpublished lanes as public performance results. They become public claims only after a real benchmark suite report is exported to `assets/data/benchmark_latest.json` and the snapshot table above is updated.

## Lane Scores Overview

```vegalite
{
  "$schema": "https://vega.github.io/schema/vega-lite/v5.json",
  "data": {"url": "../assets/data/benchmark_latest.json"},
  "mark": {"type": "bar", "cornerRadiusEnd": 4, "tooltip": true},
  "encoding": {
    "y": {"field": "lane_id", "type": "nominal", "sort": "-x", "title": "Lane"},
    "x": {"field": "score", "type": "quantitative", "title": "Score"},
    "color": {"value": "#355caa"},
    "tooltip": [
      {"field": "lane_id", "title": "Lane"},
      {"field": "lane_name", "title": "Description"},
      {"field": "score", "title": "Score", "format": ".1f"},
      {"field": "weight", "title": "Weight", "format": ".1f"}
    ]
  },
  "width": "container",
  "height": 250,
  "title": "Weighted Lane Scores"
}
```

## Frame Timing

```vegalite
{
  "$schema": "https://vega.github.io/schema/vega-lite/v5.json",
  "data": {"url": "../assets/data/benchmark_latest.json"},
  "mark": {"type": "bar", "cornerRadiusEnd": 4, "tooltip": true},
  "encoding": {
    "y": {"field": "lane_id", "type": "nominal", "sort": "-x", "title": "Lane"},
    "x": {"field": "p99_frame_ms", "type": "quantitative", "title": "Frame Time (ms)"},
    "color": {"value": "#355caa"},
    "tooltip": [
      {"field": "lane_id", "title": "Lane"},
      {"field": "p99_frame_ms", "title": "P99 Frame (ms)", "format": ".2f"},
      {"field": "avg_fps", "title": "Avg FPS", "format": ".1f"},
      {"field": "gpu_time_frame_ms", "title": "GPU Time (ms)", "format": ".2f"}
    ]
  },
  "width": "container",
  "height": 250,
  "title": "P99 Frame Time by Lane (lower is better)"
}
```

## How to Update

1. Run a benchmark: `python tests/runtime/run_benchmark.py --profile everything`
2. Export data: `python scripts/export_benchmark_vegalite.py`
3. Update the current snapshot table above when the published lane set changes.
4. Build docs: `python scripts/build_docs_site.py --strict`

See [Benchmark Suite Runner](../testing/benchmark-suite.md) for full benchmark documentation.
For benchmark invocation flags and CI lanes, see [Build / Test / CI Reference](../reference/build-test-ci.md).

## Runtime Diagnostics and Caching

The module ships a handful of runtime knobs that are intended for users tuning
startup cost and warm-cache behaviour. All of them are surfaced as
ProjectSettings under `rendering/gaussian_splatting/...` and ship with sensible
defaults; nothing here needs to be enabled to render correctly.

### Startup trace

`rendering/gaussian_splatting/diagnostics/startup_trace` (bool, default `true`).

When enabled, each `GaussianSplatNode3D` asset open emits one `[StartupTrace]`
log line on the first rendered frame. The line itemises the cost of init in
roughly fifteen named phases (module register, device request, shader compile,
streaming buffer alloc, atlas build, first-frame raster pipeline create, payload
parse, etc.) plus a `total=` end-to-end duration.

When disabled, the macro is a static-atomic short-circuit with no measurable
overhead, so leaving it on is the default for development builds and benchmarks.

Full output format, phase reference, and consumer-script guidance:
[Startup Trace](startup-trace.md).

### SPIR-V disk cache

The module persists compiled shader binaries to disk so subsequent module
loads can skip the GLSL-to-SPIR-V compile step on a cache hit. The cache is
keyed on shader source, sorted preprocessor defines, and a device fingerprint
(vendor, device name, API/version, pipeline-cache UUID), so driver upgrades
and GPU swaps invalidate automatically without manual housekeeping.

Settings:

| Key | Type | Default | Notes |
| --- | --- | --- | --- |
| `rendering/gaussian_splatting/cache/spirv_cache_enabled` | bool | `true` | Master switch. |
| `rendering/gaussian_splatting/cache/spirv_cache_max_mb` | int | `64` | LRU cap across all device subdirs (range 4-1024). |

Storage: `user://gsplat_spirv_cache/<device-hash>/<key>.spv` (one subdir per
GPU). On module init the cache is pruned to fit `spirv_cache_max_mb` using an
LRU policy keyed on file mtime; cache hits also touch the file so frequently
loaded shaders are not evicted ahead of cold entries. Stores go through a
`.tmp` write + rename with `.bak` rollback, so a crash mid-write cannot lose
a previously cached blob.

When to disable: only if you suspect a stale or corrupt blob is being served
(e.g. after an out-of-tree shader patch that did not bump the cache version).
Toggle the setting to `false`, restart, and the next compile will run from
source. The cache is safe to delete by hand at any time.

### Streaming persistent buffer sizing

The streaming path allocates a single persistent GPU storage buffer sized
once at asset init. It is now sized from the actual asset's chunk count plus
a 25 percent headroom (with a floor of `STREAMING_DEFAULT_MIN_CHUNKS_IN_VRAM`
and a ceiling at the regulated `effective_max_chunks`), rather than always
allocating the full regulated maximum. Eviction pressure can grow the buffer
on demand via `_grow_persistent_buffer()`, which copies the live region to a
larger allocation in-place.

Surfaced metrics (read via `RenderDiagnosticsOrchestrator`):

- `streaming_initial_capacity` - chunks reserved on init
- `streaming_current_capacity` - chunks the persistent buffer currently fits
- `streaming_grow_count` - times the buffer has grown since init

See [Memory Subsystem Guide](../../modules/gaussian_splatting/MEMORY_SUBSYSTEM.md)
for the budget regulator and eviction policy that drive growth.

### First-frame raster pipeline pre-create

`rendering/gaussian_splatting/init/eager_raster_pipeline` (bool, default `true`).

The graphics raster pipeline is now built at `TileRenderer` init rather than
lazily on the first dispatch, removing a ~tens-of-ms first-frame stall. The
savings show up in the startup trace as a missing or near-zero
`first_frame_raster_pipeline_create` phase.

If the eager pre-create binds the wrong framebuffer format (rare; only when
the caller-provided format hint disagrees with the real framebuffer built on
first frame), the lazy reformat path inside `dispatch_tile_rasterizer()` frees
the eager pipeline and rebuilds it, so correctness is preserved. Disable the
setting only if you are debugging that fallback path.

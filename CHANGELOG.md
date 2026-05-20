# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- Versioned GitHub Pages docs pipeline with media LFS support
- Single-process benchmark orchestrator for unified scene benchmarking
- Visual metrics to synthetic benchmark scenes
- Shader dependency guard and async readback test hardening
- Tile shared-memory raster contract enforcement in editor
- RWLock migration for gaussian core data
- Streaming queue pressure controller and scan throttle
- LOD distance-based chunk reduction (Octree-GS approach)
- Multi-asset atlas registration for streaming
- Per-chunk quantization (Unity-inspired 4x compression)
- Animation state machine with keyframe interpolation and clip blending
- Color grading bake/restore workflow on GaussianSplatNode3D
- Documentation: streaming feature guide, animation feature guide, expanded performance presets
- Renderer diagnostics destroy hook plus cull/sort short-circuit on empty frames (#331)
- Visual-compare helper for compositor regression captures (#332)
- GPU test harness Phases 1-3 with `--gs-gpu-test` entrypoint, `tests/ci/run_gpu_harness.py` supervisor, doctest visual lane in `.github/workflows/baseline_qa.yml`, and seeded captures under `tests/visual_baselines/` (#333)
- Per-phase startup-time trace via `GS_STARTUP_SCOPE` RAII timer and `[StartupTrace]` log lines, gated by `rendering/gaussian_splatting/diagnostics/startup_trace` (default on) (#342)
- Persistent SPIR-V disk cache (`modules/gaussian_splatting/renderer/spirv_disk_cache.{cpp,h}`) (#343)
- Pre-created graphics raster pipeline at init to eliminate first-frame stall (#345)

### Changed

- Benchmark scenes unified under single-process orchestrator
- Streaming path uses shared `gs::settings` helpers (ISSUE-018)
- Removed friend-only access in streaming orchestrator (ISSUE-019)
- Instance sort cache preserved across strict-mode toggles
- Aligned scons arguments between `gpu-tests` and `gpu-harness` lanes (#338)
- `REQUIRE_GPU_DEVICE()` owns its fallback `RenderingDevice` lifetime via `ScopedFallbackRD` (#340)
- RID-leak listener threshold raised to 4 MiB and the gate now fails on leaks (#341)
- Right-sized streaming persistent buffer (#344)

### Fixed

- Duplicated mike version/alias in docs deploy workflow
- GDScript const compatibility in benchmark scripts
- Missing `ColorGradingResource` include and `batches_completed` getter
- Trailing whitespace in sorting pipeline
- CPU fallback and OneSweep support in sorting validation
- Compositor R/W hazard from compute `imageLoad` on live destination (scratch-copy path) — #256 black-blocks regression (#332)
- doctest argv ownership in GPU test harness via RAII (`gs_gpu_test_runner.cpp`, `tests/test_main.cpp`) (#339)

# Tests & Build System — Deep Audit

**Scope:** `modules/gaussian_splatting/tests/` (~33.5k LOC, 81 files), `modules/gaussian_splatting/SCsub`, `config.py`, `register_types.{cpp,h}`, `doc_classes/` (14 XMLs), root `tests/ci/`, `tests/runtime/`, `tests/fixtures/`, `tests/examples/`, `BUILDING.md`, `CMakeLists.txt`, module README/ARCHITECTURE/MEMORY\_SUBSYSTEM/READING\_ORDER/ABBREVIATIONS/RIDER\_\*.md, the four `.github/workflows/*.yml` files.

**Method:** sampled 15 test files (all four >1k-LOC `.h` files, all 14 test `.cpp` files, eight smaller `.h` samples), read every build file end-to-end, greped assertion/SKIP/TEST\_CASE counts across the tree, cross-referenced SCsub + `modules/SCsub` registration logic, read all four CI workflows fully.

## Summary

**Tests: D+. Build system: C+.** The test catalogue is large and the doctest harness is well integrated with Godot's `modules_tests.gen.h` mechanism. But the codebase has a structural bug that silently erases \~60% of the visible test coverage, and a central CI guard is built around working around that bug instead of fixing it. The build is otherwise defensively well-engineered — explicit manifests, ABI `static_assert`s, deterministic-fixture validation, AVX2 autoprobe, multi-layered CI guards — but the default-off `tests=yes`, the optional shader validator, and the implicit phantom-TEST\_CASE failure mode add up to an uneasy "ship it".

Headline findings (evidence inline in "Top issues"):

1. **Phantom tests.** Godot's module test aggregator (`modules/SCsub:48`) only globs `tests/*.h` into `modules_tests.gen.h`. But ~50% of Gaussian Splatting's `TEST_CASE` macros live in `.cpp` files that go into the module static library; their static registrars are dead symbols from the linker's POV and never register. `tests/ci/run_module_tests.py:34-52` documents this as a known trap ("Phantom tags (zero runtime tests) must NOT appear here because strict lanes fail on zero coverage") and works around it by marking entire lanes advisory (`strict=False`). At least **250+ TEST\_CASE/SUBCASE entries spread across 14 `.cpp` files** are affected.
2. **4 baseline failures undocumented.** `project_test_baseline.md` (user memory) says 4 GS tests have been failing on `d78d180c70`. The repo itself contains *zero* references to these tests — no quarantine list, no `ignored_tests.txt`, no skip-marker. They are dark-matter failures.
3. **`tests=yes` is not the default.** `SConstruct:212` — `BoolVariable("tests", ..., False)`. Every developer local build ships without tests; CI has to opt-in per-lane; the Linux lane of `release_builds.yml` does *not* pass `tests=yes` at all for the Linux half of the release.
4. **Shader validator hook is dead in CI.** `gs_validate_shaders` defaults falsy in `SCsub:119-120`. No workflow sets it (grep returned zero hits in `.github/workflows/`). `gaussian_shader_validation.yml` uses `compile_shaders.py` directly — the SCsub hook is a parallel, untested path.
5. **2,178 LOC of orphan infrastructure.** `memory_validator.cpp` (1,374 LOC) and `performance_benchmark.cpp` (804 LOC) contain zero TEST\_CASE macros. They're declared, built into the module, and — judging by the doctest link surface — unreferenced from any active lane. This is billable compile time with unknown payoff.

Grade split: **Test design: D** (lots of effort but 50% dark coverage and 4 muted failures). **Test framework/CI: C+** (the guard scripts are genuinely good; the runner is clever; the lane sharding is sensible; but it's papering over #1). **Build hygiene: B-** (explicit manifests, ABI asserts, metadata consistency guard — but see CMake GLOB caveats and dead SCsub code).

## What this code does

**Test inventory** (81 files, 33,533 LOC, 456 TEST\_CASE, 3,353 CHECK/REQUIRE asserts):

- **28 `.cpp` files, 13,773 LOC** — some with doctest TEST\_CASEs (phantom), some pure infrastructure:
  - Infrastructure (no TEST\_CASEs): `memory_validator.cpp`, `performance_benchmark.cpp`, `run_gpu_validation.cpp`, `test_gpu_integration.cpp`, `test_utils.cpp`, the 8 `synthetic_*.cpp` generators.
  - Phantom test hosts (TEST\_CASEs in `.cpp`, not registered): `test_gpu_streaming.cpp` (49 TEST\_CASEs), `test_gpu_sorting.cpp` (8), `test_integration.cpp` (11), `test_lod_system.cpp` (8), `test_tile_renderer.cpp` (8), `tile_renderer_regression_test.cpp` (1), `test_tile_prefix_scan_utils.cpp` (5), `test_tile_prefix_scan_renderer_limit.cpp` (2), `test_tile_async_readback_freshness.cpp` (4), `test_gaussian_streaming_lifecycle.cpp` (6), `test_phase1_integration.cpp` (4), `test_painterly_material.cpp` (3), `test_painterly_viewport_copy.cpp` (1), `test_asset_dependency_manager.cpp` (4), `test_config_validation.cpp` (empty ~10 LOC), `test_gaussian_splatting.cpp` (1 — included from `test_gaussian_splatting.h` via side-include, but even that goes through a `.cpp`).
- **53 `.h` files, 19,760 LOC** — the only files whose TEST\_CASEs are actually registered (via `modules/SCsub:48` glob). Largest: `test_renderer_pipeline.h` (4,086 LOC, 70 TEST\_CASE, 493 CHECK/REQUIRE), `test_gaussian_splat_node.h` (2,371 LOC, 36 TEST\_CASE), `test_gaussian_importer.h` (2,131 LOC, 46 TEST\_CASE), `test_scene_director_submission_scaffolding.h` (1,618 LOC, 18 TEST\_CASE).
- **Assertion density:** 3,353 asserts / 33,533 LOC ≈ **1 per 10 LOC** — above the 1/20 threshold. But see caveats in coverage section (skips and advisory lanes dilute effective density).

**Test categories mapped to `run_module_tests.py` lanes** (`tests/ci/run_module_tests.py:63-109`): 16 strict headless lanes by tag (`[Animation]`, `[ComputeInfra]`, `[Config]`, `[Container]`, `[DynamicInstance]`, `[Editor]`, `[Importer]`, `[Node]`, `[PLY]`, `[Persistence]`, `[SceneTree]`, `[SortBenchmark]`, `[Synthetic]` (non-strict), `[VRAMBudgetRegulator]`, `[ViewTransform]`, `[WorldIO]`). Plus 4 advisory lanes for the phantom-tag TEST\_CASEs: `TileRenderer`, `GPU Memory Stream`, `Streaming Pipeline`, `GaussianSplatting [untagged]`. One opt-in `[RequiresGPU]` lane that always skips because `--test` mode doesn't initialize a RenderingDevice.

**Static guards** (`tests/ci/run_module_tests.py`): tracked backup-file guard, tracked-artifact hygiene guard (regex blocklist for `__pycache__`, `*.pyc`, stray PNG dumps, runtime logs; approves synthetic PLY fixtures from `prepare_synthetic_assets.py`'s canonical spec), build-metadata consistency guard, shader-dependency contract guard, Gaussian-layout sync guard, benchmark-asset-path guard, render-path mutation guard (blocks `set_setting`, `ResourceSaver::save`, `FileAccess::WRITE`, `DirAccess::make_dir_recursive`, `store_*` etc. from landing in `renderer/` without explicit guard tokens), 3 static-format regex guards for render pipeline code paths, history-artifact audit guard (`off/warn/strict`), synthetic-asset preparation step.

**Build overview:**
- `modules/gaussian_splatting/SCsub` — explicit `source_directories` list (13 dirs), conditional editor/tests inclusion, AVX2 auto-detect via try-compile, optional `-march=native`, optional `gs_silence_logs` for perf runs, optional `gs_validate_shaders` stamp-file hook.
- `modules/gaussian_splatting/config.py` — rejects build on `c++11/c++14` (requires C++17+), platform allow-list `{linuxbsd, windows, macos}`, disables on `disable_3d=yes`.
- `modules/gaussian_splatting/CMakeLists.txt` — IDE-only, file(GLOB) sources, not authoritative.
- `register_types.cpp` — single-file class registration with singleton bootstrap for `GaussianSplatManager` and `GaussianSplatSceneDirector`, plus resource format loaders/saver, plus editor importers.

**CI workflows:**
- `release_builds.yml` — Linux (ubuntu-latest) + Windows (self-hosted `[Windows, X64, godotgs]`) build + package. Nightly tag `nightly-YYYYMMDD`.
- `baseline_qa.yml` — Windows GPU (`[self-hosted, Windows, X64, godotgs, gpu]`) + Linux CPU lanes, runs `tests/ci/run_baseline_qa.py` with categories.
- `gaussian_production_gates.yml` — guards, GPU-evidence-requirement decision, module-validation build+test, runtime-validation profiles, openworld-proof benchmark lanes.
- `gaussian_shader_validation.yml` — standalone `compile_shaders.py` runner, self-hosted Windows, requires `glslc`/`glslangValidator`.

## Test coverage assessment

**Where coverage is actually solid (registered `.h` tests):**
- **Config validation** (`test_config_validation.h`, 810 LOC, 30 TEST\_CASE, 185 CHECK) — exhaustive GPUSortingConfig validation matrix, per-setting sweep, default-value sanity.
- **Importers** (`test_gaussian_importer.h`, 2,131 LOC, 46 TEST\_CASE, 285 CHECK) — PLY/SPZ/.gsplatworld round-trip, malformed input rejection, metadata propagation.
- **Node lifecycle** (`test_gaussian_splat_node.h`, 2,371 LOC, 36 TEST\_CASE, 365 CHECK) — scene tree integration, editor-side paths, SceneTree teardown.
- **Persistence round-trip** (`test_persistence_roundtrip.h`, 787 LOC, 16 TEST\_CASE, 74 CHECK) — chunk header rewriting, version tampering, serializer stability.
- **Renderer pipeline ABI** (`test_renderer_pipeline.h`, 4,086 LOC, 70 TEST\_CASE, 493 CHECK) — `static_assert` GPU layout contracts (`InstanceDataGPU`, `AssetMetaGPU`, `ChunkMetaGPU`, `PackedGaussian`, `TileRenderParamsGPU` sizes and field offsets — 24 contracts in lines 39-63 alone). Strong guard against silent ABI drift.
- **Scene Director scaffolding** (`test_scene_director_submission_scaffolding.h`, 1,618 LOC, 18 TEST\_CASE, 363 CHECK) — instance submission lifecycle.
- **Synthetic-generator determinism** (`synthetic_baselines/*.json`, `generate_synthetic_splat_baselines.py`) — stored hashes (`config_hash`, `scene_hash`) for uniform/clustered generators, `--check` mode catches drift.

**Coverage gaps (none of these exist as real tests in the current registration):**
- **Everything in `test_gpu_streaming.cpp` (49 TEST\_CASEs).** Streaming pipeline concurrency, chunk snapshot coherence under mutation, stale-generation drop, residency finalize — all phantom.
- **GPU sorter correctness.** `test_gpu_sorting.cpp` has 8 TEST\_CASEs all tagged `[RequiresGPU]` — both phantom (in `.cpp`) AND skipped by `--test` mode. Net coverage: zero.
- **TileRenderer core.** `test_tile_renderer.cpp` (8 TEST\_CASEs) + `tile_renderer_regression_test.cpp` (1 TEST\_CASE, 1,406 LOC of setup) + `test_tile_async_readback_freshness.cpp` (4) + `test_tile_prefix_scan_utils.cpp` (5) + `test_tile_prefix_scan_renderer_limit.cpp` (2) — ~3,200 LOC of phantom.
- **LOD integration.** `test_lod_system.cpp` (8 TEST\_CASEs, 686 LOC) — phantom. Also uses a stale `GaussianData` layout (`splat.color`, `splat.index` — see Top Issues).
- **Painterly material.** `test_painterly_material.cpp` (3) + `test_painterly_viewport_copy.cpp` (1) + `test_painterly_pipeline.h` (2) — only the `.h` ones register.
- **Memory/perf infrastructure unused.** `memory_validator.cpp` (1,374 LOC) and `performance_benchmark.cpp` (804 LOC) classes exist but no `.h` test header reaches them as of this audit.
- **4 undocumented baseline failures.** Per `project_test_baseline.md` user memory, 4 tests fail on base `d78d180c70`. The repo contains no in-tree record, no skip list, no tracking issue link, no CI `continue-on-error` quarantine. `docs/reports/ci_release_audit_2026-04-16.md:100` suggests at least one is in `[GaussianSplatting][Editor]`. Someone fixing a similar issue would have no way to know they're fixing a real regression vs. triggering a dormant bug.

**Effective assertion density**, after correcting for phantom TEST\_CASEs: registered `.h` files carry ~2,300 of the 3,353 CHECK/REQUIRE macros across 19,760 LOC → ~1 per 8.6 LOC for code that actually runs — genuinely good. The headline 1/10 number is diluted by phantom tests.

**Skip-marker density:** 299 matches for `MESSAGE("Skipping...`" across 14 files (`test_renderer_pipeline.h` alone has 139). Many headless lanes degrade gracefully when no RenderingDevice exists. This is reasonable, but it means the strict-mode lane criteria (`passed_tests > 0 AND passed_asserts > 0`) is brittle — a CI environment change that silently flips more tests to "skip" would pass as green.

## CI pipeline

**Is `tests=yes` the default?** **No.** `SConstruct:212` — `opts.Add(BoolVariable("tests", "Build the unit tests", False))`. Consequences:
- Local dev builds have zero tests unless explicitly requested.
- `release_builds.yml:214` (Linux lane) builds WITHOUT `tests=yes` — the published Linux binary cannot run `--test`.
- `release_builds.yml:326-329` (Windows lane) adds `tests=yes` *only* for non-stable channels. Stable Windows binaries also cannot run `--test`.
- `baseline_qa.yml:87`, `gaussian_production_gates.yml:255,412` (Windows GPU + module-validation + openworld-proof lanes) all explicitly pass `tests=yes`. Good.
- `baseline_qa.yml:242` (Linux CPU lane) passes `tests=yes`. Good.

**Where tests actually run:**

| Workflow | Job | Builds with tests | Test runner | Notes |
|---|---|---|---|---|
| `gaussian_production_gates.yml` | `module-validation` | Yes (self-hosted Windows+GPU) | `run_module_tests.py --godot-binary <bin>`; plus `run_baseline_qa.py --category pipeline`; plus two `run_runtime_validation.py` profiles | Main PR gate |
| `gaussian_production_gates.yml` | `guards` | N/A | `run_module_tests.py --guard-only` | Fast pre-build check |
| `gaussian_production_gates.yml` | `openworld-proof-evidence` | Yes (self-hosted Windows+GPU) | `run_benchmark.py` (non-blocking, `continue-on-error: true`) | Schedule + manual |
| `baseline_qa.yml` | `gpu-tests` | Yes (self-hosted Windows+GPU) | `run_baseline_qa.py --categories sorting` | PR + nightly |
| `baseline_qa.yml` | `cpu-tests` | Yes (ubuntu-latest) | `run_baseline_qa.py --categories ply,pipeline,runtime,module` | Skipped on PR! (`if: github.event_name != 'pull_request'`) |
| `release_builds.yml` | `build_linux` | **No** | (build only) | Linux release binary has no `--test` support |
| `release_builds.yml` | `build_windows` | Yes except stable | (build only) | Not exercised by a test runner in this workflow |
| `gaussian_shader_validation.yml` | `shader-validation` | N/A | `compile_shaders.py` direct | Standalone, no SCsub involvement |

**Perf-regression baselines:**
- `modules/gaussian_splatting/tests/synthetic_baselines/{clustered_seed_84,uniform_seed_42}.json` — deterministic hashes for synthetic splat generators (functional, not timing).
- `tests/fixtures/benchmark_asset_manifest.json` — asset path manifest (not perf data).
- `tests/runtime/runtime_scenarios.json` — runtime scenario catalogue.
- `README.md` "Performance Baselines" table (lines 132-142) documents targets (100K splats < 16.67ms, GPU sort < 2.0ms, upload < 1.0ms, GPU memory < 500MB, painterly resolve < 2.5ms). **These are in docs, not in code.** There is no `perf_baseline.json` / `benchmark_comparator.py` infrastructure to fail CI on regression. `performance_benchmark.cpp` infrastructure exists but isn't wired to a lane.
- `tests/ci/qa_results.json`, `baseline_qa_results.json` — per-run output, not stored baselines.
- No CI step blocks on wall-clock perf regression anywhere in the four workflows.

**CI correctness observations:**
- `gaussian_production_gates.yml` guards are correctly ordered: metadata consistency → shader deps → Gaussian layout → benchmark asset paths → render-path mutation → static format. Each is a hard fail.
- `run_module_tests.py:690-707` parses doctest output with two regexes — if summary lines are absent, that's treated as a failure (`missing doctest summary in output`). Good.
- `run_module_tests.py:962-970` fails in strict mode on "skipped doctest marker in CI" — defensive.
- `module-validation` job requires `--fail-on-skip` (lines 302, 313) for both runtime harness and streaming GPU gate. Good.
- `gaussian_production_gates.yml:187-203` — `baseline_qa.yml:cpu-tests` is skipped on PRs (`github.event_name != 'pull_request'`). This is deliberate (Windows GPU is the fast gate for PRs) but means the full Linux CPU suite only runs on master/develop push and nightly. Regressions visible only to Linux CPU lanes can land before they're caught.

## Strengths

- **Explicit source manifest** in `SCsub:176-191`. No reckless `glob('**/*.cpp')`. Build-metadata consistency guard (`check_build_metadata_consistency.py`) enforces that SCsub/CMakeLists.txt/config.py/doc\_classes agree.
- **ABI contract layer.** `test_renderer_pipeline.h:39-63` has 24 `static_assert` statements pinning GPU struct layouts at compile-time. Any accidental padding/alignment change fails the build.
- **Render-path mutation guard** (`run_module_tests.py:270-316`). Diff-based git hook that blocks `set_setting`, `ResourceSaver::save`, filesystem writes from landing in `modules/gaussian_splatting/renderer/` unless annotated with `GS_CI_ALLOW_RENDER_PATH_SETTING_MUTATION` / `GS_CI_ALLOW_RENDER_PATH_FS_WRITE`. This is a strong defensive pattern against the exact "renderer silently writes to user project" bugs that killed earlier releases.
- **Deterministic synthetic fixtures** (`synthetic_baselines/*.json`) with a `--check` mode wired into `run_phase1_tests.py`. Good drift protection.
- **Static format safety guards** (`run_module_tests.py:156-183`) — three regex-based invariants on `render_pipeline_stages.cpp`, `render_output_orchestrator.cpp`, `output_compositor.cpp` that catch known-broken format-resolution patterns.
- **Doctest lane sharding** (`run_module_tests.py:63-109`) splits one huge doctest run into 17 deterministic subsets so a crash in one area doesn't erase summary output for the rest. Genuinely clever defensive engineering.
- **AVX2 try-compile autodetect** (`SCsub:51-89`) — better than unconditional `-mavx2`. Works with both MSVC and GCC/Clang.
- **Git-history artifact guard** (`run_module_tests.py:557-623`) — catches committed-then-pruned build artifacts (`*.pyc`, stale PNG dumps) via `history_artifact_audit.py`. Three modes: off/warn/strict.

## Top issues

Severity taxonomy: **crash** = runtime fault or CI break; **corruption** = silent wrong behavior; **perf** = build/run time waste; **maint** = accumulated tech debt or landmines.

1. **\[severity: corruption\]** `modules/SCsub:48` + every `modules/gaussian_splatting/tests/test_*.cpp` file containing `TEST_CASE` — Godot's module-test header registrar globs only `*.h`; doctest static registrars in `.cpp` files are dead symbols inside the module static library and the linker strips them, so those test cases never register — matters because ~50% of the visible test inventory (at least 13 `.cpp` files, 250+ TEST\_CASEs including all streaming-pipeline concurrency tests, all tile-renderer regression tests, all GPU-sorting correctness tests) is phantom and reports zero runtime coverage — fix direction: either (a) move all TEST\_CASEs into `.h` files and delete the `.cpp` equivalents, or (b) add `-Wl,--whole-archive` or equivalent linker flag for the test build to force all symbols, or (c) teach `modules/SCsub` to generate a `tests_autoregister.gen.cpp` that explicitly references a dummy symbol per `.cpp` test file. Repo-wide change, but the status quo silently lies about coverage. **This is the single largest issue in this unit.**
2. **\[severity: corruption\]** `modules/gaussian_splatting/tests/` + repo-wide — 4 pre-existing test failures tracked only in user memory (`project_test_baseline.md`), not in a `QUARANTINE.md`, `.doctest-skip`, `continue-on-error` list, or tracking issue in the repo — matters because new contributors and CI have no reproducible definition of "passing"; a regression into any of these 4 tests would look identical to the pre-existing failure — fix direction: add `modules/gaussian_splatting/tests/QUARANTINE.md` with explicit test-case names, Godot commit hash they were first observed on, linked tracking issue, and either (a) quarantine them via `--test-case-exclude` in `run_module_tests.py` or (b) mark their lanes `strict=False` with a TODO.
3. **\[severity: maint\]** `SConstruct:212` — `BoolVariable("tests", ..., False)` — tests are off by default, local dev builds silently ship with no test harness, `release_builds.yml:214` (Linux) builds the release binary with no `--test` at all — matters because discovery of test failures happens only on self-hosted Windows CI (see CI table) and the published Linux binary literally cannot self-test — fix direction: flip default to `True` (mirrors Godot upstream practice since 4.x) or at minimum add `tests=yes` unconditionally to `release_builds.yml:214`.
4. **\[severity: maint\]** `modules/gaussian_splatting/SCsub:117-170` + `.github/workflows/*.yml` — the `gs_validate_shaders=yes` SCsub build hook defaults to off and no CI workflow ever sets it; `gaussian_shader_validation.yml` uses `compile_shaders.py` directly as a separate workflow — matters because the SCsub hook is ~50 lines of dead code in practice; developers reading SCsub will assume build-time shader validation is on the hot path when it isn't — fix direction: either wire it into one CI lane for coverage or delete the hook and keep `compile_shaders.py` as the single validator.
5. **\[severity: perf\]** `modules/gaussian_splatting/tests/memory_validator.cpp` (1,374 LOC) + `performance_benchmark.cpp` (804 LOC) — zero TEST\_CASE macros, full classes compiled into every `tests=yes` build, no `.h` test file was observed reaching them via sampled includes — matters because ~2,178 LOC is compile-time and link-time overhead on every tests-enabled build with no confirmed runtime consumer — fix direction: grep for actual callers; if unused, delete; if "planned future use", gate behind an `#ifdef GS_EXTENDED_TESTS` with explicit opt-in.
6. **\[severity: corruption\]** `modules/gaussian_splatting/tests/test_lod_system.cpp:27-46` — the test writes `splat.color`, `splat.index` on a struct named `GaussianData`, but the production struct (see `modules/gaussian_splatting/core/gaussian_data.h` via `register_types.cpp:14`) uses `sh_dc` of type `Color` and no `index` field; the file is included by header `test_lod_system.h` which is registered, but won't compile against current data model — matters because either this is currently broken (silently excluded from build), or it's operating on a stale shadow class that isn't what the renderer uses — fix direction: open the file, either rewrite against the real `Gaussian` struct from `core/gaussian_data.h`, or delete both `.cpp` and `.h` if superseded. (Context: issue #1 means this `.cpp` is phantom anyway; but the `.h` still references the orphaned types.)
7. **\[severity: maint\]** `modules/gaussian_splatting/CMakeLists.txt:82-189` — 18 separate `file(GLOB ...)` calls with a warning comment at line 1-5 admitting CMake doesn't auto-detect new sources — matters because IDE users (Rider) get stale intellisense whenever they add a `.cpp` without re-running CMake configuration; anyone reading the CMakeLists will reasonably assume it's the authoritative build and it silently drifts from SCsub — fix direction: replace each `file(GLOB)` with an explicit list mirroring `SCsub`'s `source_directories` + per-directory manifest. The build-metadata consistency guard already enforces they match; this would make the CMake side actually authoritative for its IDE role.
8. **\[severity: maint\]** `.github/workflows/baseline_qa.yml:187-189` — `cpu-tests` lane explicitly `if: github.event_name != 'pull_request'` — matters because the full Linux-CPU ply/pipeline/runtime/module categories only run on master/develop push and nightly; a Linux-only regression can land via PR and not be observed until nightly — fix direction: either re-enable for PRs (accept the extra time cost) or explicitly document the "Windows-GPU is the fast gate" decision in the workflow header comment so reviewers know what's skipped.
9. **\[severity: maint\]** `modules/gaussian_splatting/tests/run_gpu_validation.cpp:1-40` — self-documented as "Usage: godot --headless --script run\_gpu\_validation.cpp" but a `.cpp` isn't a Godot script; the file is compiled into the test build but has no TEST\_CASE and no obvious entry point — matters because it's 399 LOC of code with an unreachable invocation path; likely dead code from an older test harness — fix direction: delete or convert to a proper `.gd` script + `.cpp` harness.
10. **\[severity: corruption\]** `tests/ci/run_module_tests.py:963-999` — when `passed_tests <= 0 AND passed_asserts <= 0` and the lane has skip markers, strict lanes fail; non-strict lanes continue advisory. But the strict lanes are selected by tag (`[Animation]` etc.); if every test in a strict lane has a `MESSAGE("Skipping ...")` early-return (because no RenderingDevice is available on a given CI runner), the lane reports `passed_tests == 0` and strict-fails even though the code is fine — matters because CI environment changes (headless driver swap, Vulkan availability change) would turn every strict GPU-adjacent lane red in ways that look like code regressions — fix direction: distinguish "skipped due to environment precondition" from "no tests found"; track the skip count as positive coverage when it matches the lane's expected-skip budget.
11. **\[severity: maint\]** `modules/gaussian_splatting/tests/test_macros.h:65-76` — `REQUIRE_GPU_DEVICE()` macro creates a *new* local rendering device every invocation via `create_local_rendering_device()` and never destroys it — matters because every TEST\_CASE that uses the macro leaks a local rendering device per test run; in a full CI run this is potentially hundreds of leaks — fix direction: use RAII, or at least `memdelete(rd)` in a shared teardown pattern.
12. **\[severity: perf\]** `modules/gaussian_splatting/SCsub:213-219` — per-directory `.glob()` iteration + per-directory `add_source_files(..., "{directory}/*.cpp")` globs every build — matters because SCons is (in principle) cache-friendly but on no-op incremental builds this still walks ~13 directories. Minor compared to MSVC compile time but observable on fast incremental rebuilds — fix direction: if scons dep-tracking already handles this (likely), ignore; otherwise gate the scan behind a file-modification check.
13. **\[severity: maint\]** `modules/gaussian_splatting/register_types.cpp:65-183` — single `initialize_gaussian_splatting_module` with a `switch (p_level)` on `MODULE_INITIALIZATION_LEVEL_SCENE` doing \~30 registrations in a specific order (`ConfigRegistry::initialize_all()` first, then data classes, then singletons, then rendering classes, then IO). Order is correct but there's no comment explaining *why* `GaussianSplatConfigRegistry::initialize_all()` must come before `GDREGISTER_CLASS(GaussianData)` or whatever the actual ordering constraint is — matters because the next refactor will reorder and quietly break singleton bootstrap — fix direction: add a comment above each group (e.g. `// MUST come first: reads ProjectSettings that later classes expect registered.`). Current code is correct, just undefended.
14. **\[severity: perf\]** `modules/gaussian_splatting/tests/test_renderer_pipeline.h` (4,086 LOC single file) + `test_gaussian_splat_node.h` (2,371 LOC) + `test_gaussian_importer.h` (2,131 LOC) — these 3 files together are ~8.6k LOC of test header included from `test_gaussian_splatting.h`, each compiled into the same TU — matters because any edit to any test data helper triggers recompile of ~8.6k LOC of transitive template instantiations; tests-enabled incremental builds are painfully slow — fix direction: split into smaller headers grouped by doctest tag, keep public helpers in one shared header.
15. **\[severity: maint\]** `modules/gaussian_splatting/tests/performance_benchmark.h:1-271` + README.md "Performance Baselines" table — README documents numerical targets (100K splats < 16.67ms etc.) that aren't enforced by any test. There's no baseline JSON, no comparator, no CI step that fails on perf regression. `performance_benchmark.cpp` infrastructure isn't wired to a lane — matters because "performance targets" that don't gate anything decay into wishlists — fix direction: either wire `performance_benchmark.cpp` into the runtime-validation profile or delete the README section.

## Cross-cutting patterns

- **"Work around the bug" CI posture.** The .cpp phantom-TEST\_CASE issue isn't fixed; instead `run_module_tests.py` marks those lanes non-strict and warns the reader with a 15-line comment block (`tests/ci/run_module_tests.py:34-52`). The guard scripts are well-written and sophisticated, but they guard around the core bug, not against it. Time invested in guards > time investing in the underlying fix.
- **Documentation exists at the wrong layer.** The 4 baseline test failures are in user memory. Performance targets are in README.md. The phantom-tag gotcha is in a Python docstring. CI decisions are in workflow YAML comments. None of these are discoverable from `tests/README.md`, `modules/gaussian_splatting/README.md`, or `BUILDING.md` — a new contributor reading the top-level docs would miss every one.
- **`tests=yes`-off-by-default is a Godot inheritance, not a module choice.** Upstream Godot sets the default at `SConstruct:212`. Worth keeping in mind that changing it has cross-engine implications. But the Linux release build skipping `tests=yes` (`release_builds.yml:214`) is a repo choice and should be revisited.
- **Contract enforcement is robust at structure boundaries** (ABI `static_assert`s, SCons/CMake/config.py metadata guard, shader dependency contract) **but weak at behavior boundaries** (4 undocumented failures, 299 skip markers with no tracked expected-skip budget, phantom TEST\_CASEs not enforced).
- **Large-file antipattern.** `test_renderer_pipeline.h` (4k LOC) + `test_gaussian_splat_node.h` (2.4k LOC) + `test_gaussian_importer.h` (2.1k LOC) + `test_scene_director_submission_scaffolding.h` (1.6k LOC) are all rolled into one test TU via `test_gaussian_splatting.h`. Incremental build times for test-enabled local dev builds will be punishing.

## Recommended refactor moves

**P0 (do before the next release):**
1. **Fix phantom TEST\_CASEs.** Force-link test .cpp registrars OR migrate TEST\_CASEs into .h files. Affected files: `test_gpu_streaming.cpp` (49 cases), `test_gpu_sorting.cpp` (8), `test_integration.cpp` (11), `test_lod_system.cpp` (8), `test_tile_renderer.cpp` (8), `tile_renderer_regression_test.cpp` (1), `test_tile_prefix_scan_*.cpp` (7 combined), `test_tile_async_readback_freshness.cpp` (4), `test_gaussian_streaming_lifecycle.cpp` (6), `test_phase1_integration.cpp` (4), `test_painterly_material.cpp` (3), `test_painterly_viewport_copy.cpp` (1), `test_asset_dependency_manager.cpp` (4), `test_gaussian_splatting.cpp` (1). **Effort:** 2-4 days per approach.
2. **Document the 4 baseline failures.** Add `modules/gaussian_splatting/tests/QUARANTINE.md` with exact `--test-case` pattern for each, link to a tracking issue each. Wire them into `run_module_tests.py` via `--test-case-exclude`. **Effort:** 2 hours once the 4 are identified.
3. **Fix `memory_validator.cpp` + `performance_benchmark.cpp` dead-code situation.** Delete if unused; wire into a lane if used. **Effort:** 0.5-1 day.

**P1 (soon):**
4. Make `tests=yes` default in the Linux release lane (`release_builds.yml:212-214`). **Effort:** 1 line.
5. Audit `test_lod_system.cpp` (`Gaussian.color`/`.index` vs current struct). Likely full rewrite or delete. **Effort:** 0.5-1 day.
6. Add perf-regression baseline comparator OR remove the README.md perf targets. **Effort:** 2-3 days (comparator) or 15 min (delete).
7. Fix `REQUIRE_GPU_DEVICE()` macro leak (`test_macros.h:65-76`). RAII wrapper. **Effort:** 1-2 hours.
8. Decide on the SCsub `gs_validate_shaders` hook: wire or delete. **Effort:** 0.5 day.

**P2 (tech debt, when touching anyway):**
9. Split `test_renderer_pipeline.h` (4k LOC) into per-tag headers. **Effort:** 1 day.
10. Replace CMakeLists.txt `file(GLOB)` with explicit manifests mirroring `SCsub`. **Effort:** 0.5 day.
11. Add init-order comments to `register_types.cpp`. **Effort:** 1 hour.
12. Delete or convert `run_gpu_validation.cpp` if unused. **Effort:** 0.5 day.

## Blind spots

- **Non-test `.cpp` code.** I did not read production code (`core/`, `renderer/`, `painterly/`, etc.) beyond what was needed to understand test contracts. Some "phantom test coverage" findings may be moot if other `.h` tests transitively cover the same behavior — I didn't cross-reference every duplicate case.
- **Godot upstream test harness.** Did not read `tests/test_main.h`, `tests/test_macros.h`, `tests/test_utils.cpp` in depth — I assumed Godot's harness behaves as documented.
- **Runtime actually-executed test count.** Didn't run the test suite. The "phantom tests never register" conclusion is from static analysis of `modules/SCsub:48` + doctest library behavior (static-init in archive = linker-strippable); I didn't confirm by running `godot --test --list-test-cases | grep '\[GPU Memory Stream\]'` and comparing to `test_gpu_streaming.cpp`'s visible cases.
- **Runtime validation harness internals.** `tests/runtime/run_runtime_validation.py` and `tests/runtime/run_benchmark.py` were referenced but not deep-read; profiles like `streaming-gpu-ci` / `headless-ci` are named in workflow YAML but I didn't audit what they actually check.
- **Shader compile graph.** `modules/gaussian_splatting/shaders/SCsub` + `compute/SCsub` were referenced from the top-level SCsub but not individually read.
- **Baseline QA internals.** `tests/ci/run_baseline_qa.py` was referenced in workflows but not deep-read (800+ LOC).
- **Self-hosted runner labels.** `[self-hosted, Windows, X64, godotgs, gpu]` — I took at face value that an appropriate GPU is available; I can't verify from the repo alone.
- **Cross-module test pollution.** Godot builds multiple modules' tests into the same binary. A doctest tag collision with another module (e.g. another module using `[SceneTree]`) could mis-route filter results. Didn't audit.
- **Performance baselines in external systems.** The "no perf baselines" conclusion covers in-tree data; there may be baselines stored in a GitHub artifact cache, a tracking spreadsheet, or `baseline_qa_results.json` from a previous run used as reference — if so, that's not visible from the repo audit lens.

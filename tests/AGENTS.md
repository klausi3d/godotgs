# AGENTS.md — `tests`

Refines the root [`AGENTS.md`](../AGENTS.md) for the test and CI harness.
Full command reference: `docs/reference/build-test-ci.md`.

## Layout

- `tests/ci/` — guard checks, contract validators, baseline QA, GPU harness,
  and Python `unittest` tests (`test_*.py`). Entry point:
  `python tests/ci/run_module_tests.py` (`--guard-only` for the GPU-free lane).
- `tests/runtime/` — runtime validation harness, streaming/residency/GPU-stress
  GDScript scenarios, benchmark runner. Entry point:
  `python tests/runtime/run_runtime_validation.py --profile <profile>`.
- `tests/agentic/` — `unittest` tests for the `scripts/agentic` validators.

## Rules

- **Separate the tiers.** Structural/guard tests must stay deterministic and run
  without a GPU or a built binary where possible; runtime and performance tests
  are separate and may require hardware. Do not fold a hardware-dependent check
  into the guard lane.
- **Deterministic fixtures.** Use fixed seeds and the synthetic-asset helpers
  (`tests/runtime/prepare_synthetic_assets.py`); do not depend on wall-clock,
  network, or machine-specific paths. Real-scan visual validation is required for
  rendering-math changes but lives in its own lane, not the unit tests.
- **No unjustified baseline/threshold updates.** Never edit a golden baseline,
  performance threshold, or release-gate manifest just to make a test pass.
  Changing a baseline requires its own justification and review; treat it as a
  contract change, not a fix.
- **No generated artifacts in commits.** Reports, logs, `output/`, captured
  images, and `__pycache__` are build outputs, not source — keep them untracked.
- **Document what you ran.** PRs must list the exact test commands and their
  results. Where GPU/Windows hardware was unavailable, write "not run", never
  "passed".
- Python tests use the standard-library `unittest` style already in the repo
  (e.g. `tests/ci/test_renderer_release_gates.py`): load the module under test
  with `importlib.util.spec_from_file_location` and use `tempfile` fixtures. Do
  not add a new test-framework dependency.

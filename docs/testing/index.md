# Testing

Two parallel test lanes are maintained:

- The classic `--test` doctest lane, driven by `tests/ci/run_module_tests.py` and the baseline runners.
- The `--gs-gpu-test` GPU harness lane, driven by `tests/ci/run_gpu_harness.py` — see [Testing Setup Guide](setup-guide.md#gpu-test-harness-gs-gpu-test) and the in-module reference at `modules/gaussian_splatting/tests/README.md`.

- [Testing Setup Guide](setup-guide.md)
- [Benchmark Runner](benchmark-suite.md)
- [Unified Benchmark Scene](unified-benchmark.md)

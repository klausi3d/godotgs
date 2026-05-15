# Visual Baselines

Pixel-stable golden PNGs used by Gaussian Splatting visual-regression tests.

## How baselines are captured

Tests in the module suite that call `TestGaussianSplatting::VisualCompare::capture_and_compare()`
operate in one of two modes:

| Mode | How to trigger | Behavior |
|------|----------------|----------|
| `compare` (default) | no env var, or `GS_VISUAL_BASELINE_MODE=compare` | loads the baseline from this directory and asserts the captured frame matches within tolerance. Missing baseline = test failure with a "run in update mode" hint. |
| `update` | `GS_VISUAL_BASELINE_MODE=update` | writes the captured frame to this directory as the new baseline. Used to (re)capture after a deliberate change. |

The CI workflow `.github/workflows/baseline_qa.yml` already exposes a `baseline_mode`
job input that maps to this env var. Nightly schedule runs auto-update; PR runs compare.

## Canonical capture environment

Baselines must be captured on the project's self-hosted Windows GPU runner —
GitHub Actions label set `[self-hosted, Windows, X64, godotgs, gpu]` — because
PNG output drifts across GPUs and driver versions even when the rendering math
is identical. The same runner is what gates PRs, so a baseline captured anywhere
else will produce false failures.

When the runner's GPU driver is updated, baselines must be re-captured
deliberately. Document each recapture in the commit message:

```
visual_baselines: recapture after Vulkan driver 1.3.275 -> 1.3.296

NVIDIA Game Ready 552.22 -> 555.85 on the godotgs Windows runner.
Compared diffs against previous baselines: all changes are within
sub-LSB rendering tolerances; no functional regression.
```

## Tolerance defaults

`capture_and_compare()` defaults to:

- `max_per_channel_diff_lsb = 1.0` — at most 1 LSB difference per channel per pixel.
- `min_psnr_db = 45.0` — overall PSNR floor.

Both can be tightened per-test where the workload is deterministic enough to
justify it (e.g. solid blits, hazard repro test).

## File layout

```
tests/visual_baselines/
├── README.md                      (this file)
└── <test_name>_<W>x<H>.png        (8-bit sRGB or linear RGBA, format documented per test)
```

PNGs are committed to the repo. Keep file sizes small — prefer 256x256 or
smaller deterministic fixtures over full-viewport captures. The hazard repro
test in `tests/test_output_compositor_composite_hazard.cpp` uses 256x256.

## Related

- Helper: `modules/gaussian_splatting/tests/visual_compare.h`
- CI integration: `.github/workflows/baseline_qa.yml` (baseline_mode input)
- Aspirational scaffolding: `modules/gaussian_splatting/tests/visual_validation.h`
  has a much larger unimplemented API; treat as a planning artifact, not as
  production infrastructure. The focused helper above is what is wired up.

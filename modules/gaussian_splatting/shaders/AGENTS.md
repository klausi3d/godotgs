# AGENTS.md — `modules/gaussian_splatting/shaders`

Refines [`../AGENTS.md`](../AGENTS.md) for GLSL/compute shaders and the
host↔shader contract. Shader changes are **risk class R2** and must pass the
shader-contract checks (run by `gaussian_shader_validation.yml` and the
guard suite). See `README.md` in this directory for the compile pipeline.

## Build and includes

- Shaders are compiled to generated headers (`*.gen.h`) via `compile_shaders.py`;
  shared GLSL lives in `includes/`. If you add/rename an include or a shader,
  update the build inputs so the generated header is regenerated — a stale
  `*.gen.h` is a silent correctness bug. Do **not** hand-edit generated headers.
- Keep the shader-dependency guard happy: declared includes must match actual
  `#include`s.

## Host ↔ shader layout contract

- Struct layouts, binding indices, push-constant sizes, and shared constants must
  match **exactly** between the C++ host and the shader. A mismatch reads garbage
  or overflows. When you change one side, change the other in the same commit and
  re-run the layout-sync / contract check.
- Derive related magic numbers (binning extent, alpha cutoff, projection radius,
  low-pass dilation, etc.) from one shared constant rather than copying literals
  into both host and shader.

## Bounds and numerics

- Every buffer index in a shader must be bounds-checked or provably in range;
  guard the zero-element path. Out-of-bounds access is undefined and vendor-specific.
- Watch numeric stability: avoid catastrophic cancellation and divide-by-near-zero
  in covariance/projection math; keep precision consistent with the host.

## Cross-vendor behavior

- Code must behave on NVIDIA, AMD, and Intel — do not rely on a single vendor's
  tolerance for UB, subgroup size, or unsorted access. If a change is validated on
  only one vendor, record that as a blind spot in the PR.

## Sort keys

- Sort-key width/precision is a correctness contract (32-bit quantized keys band
  and flicker on real-scan data). Do not narrow sort keys to save bandwidth
  without re-validating visually on real-scan content.

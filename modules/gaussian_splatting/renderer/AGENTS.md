# AGENTS.md — `modules/gaussian_splatting/renderer`

Refines [`../AGENTS.md`](../AGENTS.md) for GPU resources and the render pipeline.
Renderer changes are **risk class R2** (or R3 if they cross into engine/persistence)
and require runtime/GPU evidence plus an independent GPU/performance review — see
`docs/governance/review-policy.md`.

## RenderingDevice and resource ownership

- A GPU resource (buffer, texture, uniform set, pipeline) is valid only for the
  `RenderingDevice` generation that created it. Store and compare the device
  generation before reusing a cached resource; a uniform set must never outlive
  the device/resources it binds.
- Free on teardown **and** on every failure/early-return path. New persistent GPU
  state needs an explicit, tested free path (the lifetime-accounting guards will
  flag leaks).

## Buffers, bounds, and the zero path

- Size every dispatch/copy from the actual element count; never assume a fixed
  capacity. Respect buffer bounds on both host and shader sides (see
  `shaders/AGENTS.md` for the host↔shader contract).
- Handle the **zero-element / empty-scene path** explicitly — no dispatch with a
  zero or negative count, no read past the end of a partially filled buffer.

## Synchronization and readback

- Never stall the main thread on the GPU. Use the existing async readback /
  timestamp paths; do not add a synchronous `sync()` to "fix" a race.
- **Timing honesty:** a metric must measure what its name claims. CPU dispatch
  time is not GPU time; do not report dispatch latency as GPU cost or leave a
  pass-timing monitor reading zero while implying it is measured. If a number is
  not actually measured on the render path in use, say so.

## No silent unsafe fallbacks

- Do not silently render unsorted, skip the sort, or drop records to hit a budget
  — these produce wrong images that look plausible. Any fallback must be explicit
  and observable, and must not regress correctness on real-scan content.

## Evidence

- Provide runtime/GPU evidence for render changes (the production-gates runtime
  harness, GPU harness, and/or benchmark surface). Include VRAM and frame-time
  numbers for changes that claim a performance or memory effect, measured against
  the immutable base.
- Background and references: `docs/architecture/render-pipeline.md`,
  `docs/architecture/renderer-lifetime-ownership.md`.

# Overlap Checkpoint — Closed (no divergence found)

**Closed on:** 2026-04-12
**Outcome:** No COUNT/EMIT divergence. The binning pipeline is correct end-to-end.

## Discriminating evidence

With corrected accounting (commits `0954627986`, `73fe6593e1`):

```
[DIAG-BENCH-DIVERGENCE]
  count_entered=744972    emit_entered=744972     (delta=0)
  count_accepts=1265162   emit_attempts=1265162   (delta=0)
  frame_clamped=179160
```

- `pass_delta == 0` — the same splats reach both COUNT and EMIT tile loops.
- `accept_delta == 0` — EMIT's attempted tile inserts equal COUNT's accepts.
- The sync CPU capture (`3344525169`) proved `tile_counts[t] == tile_ranges[t].y` for all 8160 tiles, including the hotspot.

## What is ruled out

- COUNT → prefix-scan → range.y handoff corruption
- EMIT rollback/atomic races
- tile-range aliasing between scan and EMIT
- per-tile binning overflow

## What the earlier "clamp=183k" signal actually was

`overflow_splats_clamped` is a frame-wide counter that also accumulates later raster-stage drops. The earlier interpretation
(`aggregated + clamped > count_accepts` ⇒ binning divergence) was an accounting error. With the correction,
`aggregated == count_accepts` exactly.

## Why this pivots the investigation

The remaining performance signal at 2M+ visible splats is the well-known fact that `max_splats_in_tile` grows into the tens of
thousands on the corridor sweep. That is a **raster complexity** problem, not a **binning bookkeeping** problem. The frame-wide
`clamped` growth reflects raster-stage clamp/drop events, which belong to the new checkpoint's scope.

## Next active checkpoint

`Raster: hotspot tiles cause unbounded per-pixel iteration cost in open-world proof lane`

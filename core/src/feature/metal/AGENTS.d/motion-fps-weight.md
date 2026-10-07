---
paths:
  - core/src/feature/metal/float_motion_metal.mm
  - core/src/feature/metal/integer_motion_metal.mm
invariant: motion_fps_weight is CPU value, per frame (ADR-1498).
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Rebase-sensitive invariants (motion_fps_weight)

- **`motion_fps_weight` = CPU's, per frame (ADR-1498)** — see canonical
  invariant note in [`../cuda/AGENTS.md`](../../cuda/AGENTS.md).
  `integer_motion_metal`, `motion_v2_metal`: `collect()` stores
  `MIN(sad / 256. / (w * h) * motion_fps_weight, motion_max_val)` (CPU
  `extract()`), `flush()` = `vmaf_motion_window_flush()` only, no own
  window. `float_motion_metal`: motion / motion2 / debug score =
  `MIN(score * motion_fps_weight, motion_max_val)`. Parity at `==`
  (`test_metal_*_parity`), not places=4.

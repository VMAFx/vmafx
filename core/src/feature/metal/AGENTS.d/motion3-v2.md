---
paths:
  - core/src/feature/metal/float_motion_metal.mm
  - core/src/feature/metal/integer_motion_metal.mm
invariant: motion3_v2 cross-twin invariant (ADR-1108).
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# motion3_v2 cross-twin invariant (ADR-1108)

- `integer_motion_v2_metal` and `integer_motion_metal` derive `motion2` /
  `motion3` with the CPU's own `vmaf_motion_window_flush()` (blend, clip,
  seed, moving average, five-frame window), never a copy. One-frame input
  -> 0 / 0 as CPU. Guard: `test_metal_motion_v2_exact_contract.py`,
  `test_metal_integer_motion_exact_contract.py`.

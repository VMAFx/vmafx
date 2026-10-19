---
paths:
  - core/src/feature/hip/integer_motion_v2_hip.c
  - core/src/feature/hip/integer_motion_v2_hip.h
  - core/src/feature/hip/integer_motion_v2/motion_v2_score.hip
invariant: motion3_v2 cross-twin invariants and score consistency must be preserved.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# motion3_v2 cross-twin invariant (ADR-1108)

- `integer_motion_v2_hip` emits `motion2_v2` / `motion3_v2` host-side in
  its flush through CPU's own `vmaf_motion_window_flush()`
  (`core/src/feature/motion_window.h`, ADR-1478, ADR-1491): blend,
  `motion_max_val` clip, `stamp_value` seed for `i < min_idx`, optional
  moving average, three-frame or five-frame window. Twin holds no copy of
  that arithmetic (CUDA / SYCL twins same; Metal still carries its copy and
  must be kept in step by hand). Guards: `test_hip_motion_v2_parity`,
  `test_hip_motion_five_frame_window`, `test_hip_exact_twins`.

---
paths:
  - core/src/feature/vif_tools.c
  - core/src/feature/vif_tools.h
invariant: lanczos4 prescale weights single implementation shared with GPU twins.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# lanczos4 Prescale Weights Shared Implementation

## `lanczos4` prescale weights have one implementation, shared with the GPU twins

`lanczos4_weights()` in `vif_tools.c` feeds both `lanczos4_interpolation()`
(CPU scaler) and `vif_scale_lanczos4_axis_weights()` (per-axis table, exported).
`speed_internal_gpu_lanczos_weights()` lays that table out for device
pipeline: 9 taps per scaled column, then per scaled row. `speed_score.cu`
reads it; no device sine (`T-GPU-SPEED-LANCZOS4-PRESCALE-DRIFT-2026-09-30`).

On rebase: upstream change to `lanczos4_kernel()`, `lanczos4_interpolation()`
or `(i + 0.5) * ratio - 0.5` position of `vif_scale_frame_lanczos4_s()`
-> same change in `vif_scale_lanczos4_axis_weights()`. Do not re-inline
weight loop into `lanczos4_interpolation()`. `test_speed_lanczos4_weights`
fails when two drift (table replay vs `vif_scale_frame_s()`, bit for bit).

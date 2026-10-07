---
paths:
  - core/src/feature/cuda/integer_motion_v2_cuda.c
  - core/src/feature/cuda/integer_motion_v2_cuda.h
invariant: Motion v2 CPU mirror contract and score emission parity.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Motion v2 mirror contract and score emission

- **`integer_motion_v2_*` mirror contract** (ADR-0662) — CPU
  `integer_motion_v2.c::mirror` maps `idx >= size` to
  `2 * size - idx - 2`. CUDA, SYCL, and Vulkan `motion_v2`
  kernels must keep that same high-edge literal. Old `-1`
  formula = stale prose from ADR-0193 bring-up, creates
  measurable CPU/GPU drift.
- **`integer_motion_v2_cuda.c::flush_fex_cuda` = CPU's window function**
  (ADR-1108, ADR-1491). `motion2_v2` / `motion3_v2` of every frame come from
  `vmaf_motion_window_flush()` (`core/src/feature/motion_window.h`, defined
  in `integer_motion.c`) over stored SAD scores: blend, `motion_max_val`
  clip, `stamp_value` for `i < min_idx`, optional moving average, three-frame
  or five-frame window. Twin holds no copy of that arithmetic; do not add one
  back (SYCL / HIP twins same; Metal still carries its copy). Options
  (`motion_blend_factor` / `motion_blend_offset` / `motion_max_val` /
  `motion_five_frame_window` / `motion_moving_average`) mirror CPU
  `VmafOption[]` rows exactly. Unlike v1 `motion_cuda` three-frame path, no
  per-frame streaming post-process.

- **`motion_v2_cuda` publishes CPU SAD score** `MIN(sad * mfw, mmxv)` in
  collect; flush hands stored values to `vmaf_motion_window_flush()`:
  no re-weighting, one-frame input and every end case decided there, as for
  CPU. `test_cuda_kernel_source_contract.py` pins it device-free: weighted,
  capped SAD in collect; call in `flush_fex_cuda`; no
  `vmaf_feature_collector_get_score` in TU; `motion_cuda` calls same
  function in `motion_flush_window()`.

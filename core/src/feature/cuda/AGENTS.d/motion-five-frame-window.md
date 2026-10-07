---
paths:
  - core/src/feature/cuda/integer_motion_cuda.c
  - core/src/feature/cuda/integer_motion_v2_cuda.c
  - core/test/test_cuda_motion_five_frame_window.c
  - core/test/motion_five_frame_twin_parity.h
invariant: motion_five_frame_window on CUDA twins = ring of three raw planes + CPU window function at flush; bit-identical.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# `motion_five_frame_window` on `motion_cuda` / `motion_v2_cuda` (ADR-1491)

- CPU contract (ADR-1478, Netflix `a2b59b77`): SAD of frame n against frame
  n-2, 0 for n < 2; `motion2` / `motion3` from
  `vmaf_motion_window_flush()` (`core/src/feature/motion_window.h`).
- Device: `ring = five ? 3 : 2`. Frame n stages into `raw[n % ring]` /
  `pix[n % ring]`, kernel reads `[(n + 1) % ring]` (frame n - (ring - 1)).
  `has_prev = index >= ring - 1`. Kernel unchanged
  (`integer_motion_sad_cuda.c`). `prev_done` = previous frame's event from
  frame 1 on, also for frame without SAD: chain of events must reach
  copy of frame n-2 (picture streams differ). Do not go back to
  `has_prev ? event : NULL`.
- Readback: only slots of frames >= `ring - 1` (`motion_readback_slots()`
  bounds in `motion_collect_batch()` / `motion_flush_pending_tail()`); slot
  below it was never zeroed or written.
- Host, `motion_cuda`, option on: `collect()` stores SAD (+ debug `motion`)
  only (`emit_batch_scores()` skips motion2 / motion3,
  `motion_collect_no_sad()` writes no motion2); `flush()` ->
  `motion_flush_window()`. Three-frame path untouched: frame-by-frame
  emission, `motion_flush_trailing()`.
- Host, `motion_v2_cuda`: `flush()` = `vmaf_motion_window_flush()` for both
  windows. No local copy of CPU flush; do not add one back.
- Option rows = CPU's (name, alias `mffw`, bool, default false,
  `VMAF_OPT_FLAG_FEATURE_PARAM`), no `VMAF_OPT_FLAG_DEFAULT_ONLY`
  (`test_gpu_option_value_capability_contract.py`,
  `test_cuda_twin_option_parity`).
- CPU change to `extract()` min_idx / prev selection or to window ->
  window comes through shared function; `ring` / `has_prev` here in same
  PR.
- Guards: `test_cuda_motion_five_frame_window` (`==`, six option sets, 11 / 1
  / 2 / 3 frames, 8 / 10 bit; 11 frames cross 8-frame batch), gate cells
  `motion_mffw`, `motion_v2_mffw` (exact, `scripts/ci/exact_twins.d/`).

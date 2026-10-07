---
paths:
  - core/src/feature/hip/integer_motion_hip.c
  - core/src/feature/hip/integer_motion_v2_hip.c
  - core/test/test_hip_motion_five_frame_window.c
invariant: motion_five_frame_window on HIP twins = two kept planes + CPU's window function at flush; bit-identical.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# `motion_five_frame_window` on `motion_hip` / `motion_v2_hip` (ADR-1491)

- CPU contract (ADR-1478, Netflix `a2b59b77`): SAD of frame n against frame
  n-2, 0 for n < 2; `motion2` / `motion3` from
  `vmaf_motion_window_flush()` (`core/src/feature/motion_window.h`).
- Device: `depth = five ? 2 : 1` kept planes (`prev_luma[]`). Frame n:
  `keep = prev_luma[n % depth]`, SAD reads it (holds frame n - depth), copy
  behind SAD on same stream overwrites it with frame n
  (`vmaf_hip_motion_sad_submit()`, unchanged). `have_prev = index >= depth`;
  no read-back without it.
- Host, `motion_hip`, option on: `collect()` stores SAD (+ debug `motion`)
  only (`msh_emit_no_sad()` for frames < depth, no `msh_emit_prev_frame()`);
  `flush()` -> `msh_flush_window()`. Three-frame path untouched.
- Host, `motion_v2_hip`: `flush()` = `vmaf_motion_window_flush()` for both
  windows. No local copy of CPU flush; do not add one back.
- Option rows = CPU's, no `VMAF_OPT_FLAG_DEFAULT_ONLY`
  (`test_gpu_option_value_capability_contract.py`,
  `test_hip_twin_option_parity`).
- Guards: `test_hip_motion_five_frame_window` (`==`, six option sets, 11 / 1
  / 2 / 3 frames, 8 / 10 bit; fixture `core/test/motion_five_frame_twin_parity.h`),
  gate cells `motion_mffw`, `motion_v2_mffw` (exact).

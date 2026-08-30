- Extended `motion_fps_weight` feature parameter to all GPU twins of
  the `integer_motion_v2` and `float_motion` families (PR #863).
  Previously the option was present only on the CPU path and the
  `integer_motion` CUDA twin (PR #851). All five backends — CUDA,
  SYCL, Vulkan, HIP, Metal — now expose `motion_fps_weight` via
  their `VmafOption options[]` table and apply it identically:
  - `integer_motion_v2_*` (flush-based motion2): weight scales both
    `score_cur` and `score_next` in `flush()` before the min.
  - `float_motion_*` (collect-based motion2): weight scales both
    `motion_score` and `prev_motion_score` in `collect()` (index >= 2)
    before the min, and scales `prev_motion_score` alone in the
    tail-emission `flush()`.
  At the default `motion_fps_weight = 1.0` the new arithmetic is a
  strict no-op; the `places=4` cross-backend parity gate is unaffected.

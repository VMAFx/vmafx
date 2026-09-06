# fix(sycl): clip integer_motion2 score to motion_max_val (1080p checkerboard drift)

In `core/src/feature/sycl/integer_motion_sycl.cpp`, `motion2_clipped = MIN(motion2 * s->motion_fps_weight, s->motion_max_val)` was computed at line 838, but the feature collector appended raw unclipped `motion2` at line 841, and debug mode appended raw `motion_score` instead of `score_clipped` at line 848. In `flush_fex_sycl`, raw unclipped `s->prev_motion_score` was appended at line 896 instead of `last_motion2`.

On the 1080p checkerboard reference pairs (`checkerboard_1920_1080_10_3_0_0.yuv` vs `..._1_0.yuv` and `..._10_0.yuv`), raw motion scores reach ~18.805 (frame 1) and ~18.858 (frame 2). With `motion_max_val=18.0`, the CPU reference clamps both frames to 18.0, yielding pooled mean `(0.0 + 18.0 + 18.0) / 3 = 12.000000`, whereas SYCL emitted unclipped scores yielding `12.554712`.

Fix: append `motion2_clipped` and `last_motion2` to the feature collector, matching the CPU reference semantics and eliminating the 1080p checkerboard drift.

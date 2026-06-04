Fix 6 reproducible test failures introduced by the recent PR train:

- `test_gpu_picture_pool_uaf`: `gpu_picture_pool.cpp` `free_p`/`fail` labels
  now set `*pool = NULL` so callers cannot UAF through a dangling handle.
- `test_motion_min_dim`: `integer_motion.c::init()` now rejects frames smaller
  than 3x3 with -EINVAL (mirrors the existing guard in `integer_motion_v2.c`).
- `test_integer_motion_coverage`: `vmaf_feature_extractor_context_extract()`
  now updates `fex->prev_ref` after a successful extract for
  `VMAF_FEATURE_EXTRACTOR_PREV_REF` extractors, matching the behaviour of
  `vmaf_read_pictures()`.  `context_destroy()` releases the held reference.
  Test corrected: `motion_five_frame_window=true` is rejected at init
  (-ENOTSUP per ADR-0337); the flush coverage is now driven without that flag.
- `test_integer_motion_v2_coverage`: `vmaf_fex_integer_motion_v2` extern
  declaration and `feature_extractor_list[]` entry restored in
  `feature_extractor.cpp` (the comment claiming it was merged into v1 was
  incorrect; the symbol lives in `integer_motion_v2.c`).
- `test_score_pooled_eagain`: `predict_load_feature_score()` now returns
  `-EAGAIN` instead of `-EINVAL` when the feature vector has not been
  created yet (e.g. `motion2_score` is only written at flush).  The test's
  streaming-pattern sub-case is corrected to flush before pooling, which
  matches the v1 implementation's batch-flush design.
- `test_framesync`: framesync verification expression fixed to compare against
  the previous frame's stored value (`2*(index-1)+2`) rather than the current
  frame's value (`2*index+2`).

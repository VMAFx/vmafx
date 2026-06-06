Fix five Ubuntu clang (CPU) + DNN CI test failures caused by `.c`/`.cpp` divergence
and missing protocol enforcement:

- `gpu_picture_pool.cpp`: set `*pool = nullptr` after OOM failure at `free_p` label
  (`.c` twin had it; `.cpp` twin used by tests did not — UAF regression).
- `feature_extractor.cpp`: guard `vmaf_dictionary_copy` with `opts_dict != nullptr`
  in `get_fex_list_entry()` (`.c` twin had the guard; `.cpp` twin did not).
- `integer_motion.c`: reject frames smaller than 3×3 in `init()` — the 5-tap Gaussian
  requires dim ≥ filter_width/2+1=3; `integer_motion_v2.c` and `float_motion.c` already
  had this check.
- `test_integer_motion_coverage.c`: replicate the `VMAF_FEATURE_EXTRACTOR_PREV_REF`
  protocol in direct `extract()` loops (set/clear `fex->prev_ref` around each call,
  mirroring `read_pictures_dispatch_one` in `libvmaf.c`).
- `predict.c`: return `-EAGAIN` (not `-EINVAL`) when a model-registered feature vector
  is not yet in the collector — the feature will be written retroactively on flush
  (e.g. motion2/motion3). Corrects `test_score_pooled_streaming_pattern` to match
  actual extractor semantics.

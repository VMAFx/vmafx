## Fixed

- **PREV_REF refcount leak in threaded batch dispatch** (ADR-1072): calling
  `vmaf_picture_unref` before `memset` on `fex->prev_ref` after `extract()`
  in `threaded_extract_batch_func` and `threaded_extract_func` balances the
  refcount bump that `feature_extractor.cpp`'s PREV_REF SWAP places on the
  current frame.  The prior bare `memset` discarded that counted reference
  without triggering the picture-pool release callback, exhausting the pool
  after ~pool_size frames and causing `vmaf_picture_pool_fetch` to deadlock
  in `pthread_cond_wait`.  `f->prev_ref` is now zeroed after the unref to
  prevent the `unref:` block from double-freeing the consumed VmafRef.
  Fixes `test_picture_pool_basic` deadlock when using PREV_REF extractors
  (e.g. `integer_motion` via `vmaf_v0.6.1` model) with n_threads > 0.

- **MS-SSIM parity test fixture size** (ADR-1072): `test_hip_ms_ssim_parity`
  and `test_cuda_float_ms_ssim_parity` used 256x144 fixtures; `float_ms_ssim`
  requires `min(w,h) >= 176` for its 5-level 11-tap Gaussian pyramid.  Raised
  fixture height to 192 to satisfy the constraint.

- **Motion parity test score channel** (ADR-1072): `test_hip_motion_parity`
  queried `VMAF_integer_feature_motion_score`, which `integer_motion` only
  emits when `debug=true`.  Added `vmaf_feature_dictionary_set("debug","1")`
  to the CPU run so the same named channel is available on both paths.

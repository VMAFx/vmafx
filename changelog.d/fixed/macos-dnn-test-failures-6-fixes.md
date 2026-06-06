## fix(test,core): 6 macOS/DNN CI failures — gpu_pool UAF, EAGAIN masking, motion min-dim, fex pool NULL, framesync seed, prev_ref

- `gpu_picture_pool.cpp`: Missing `*pool = nullptr` on `free_p:` label left caller
  with a dangling pointer on malloc failure (UAF via `vmaf_close()` teardown).
  Also missing `goto free_pic` when `alloc_picture_callback` returns an error,
  causing partial construction to remain live. The `.c` twin already had the
  `*pool = NULL` fix; the `.cpp` mirror was never updated.

- `libvmaf.c` (`vmaf_score_at_index`): `-EAGAIN` returned by
  `vmaf_feature_collector_get_score` ("score not yet available") was silently
  converted to `-EINVAL` by an unconditional fallthrough to
  `vmaf_predict_score_at_index`. Added `err != -EAGAIN` guard so transient
  "not yet ready" propagates correctly to `vmaf_score_pooled` callers.

- `integer_motion.c` (`init`): No minimum-dimension check. The 3-frame SAD
  kernel requires at least a 3x3 luma plane. Added `if (w < 3 || h < 3) return
  -EINVAL` to match the contract expected by `test_motion_min_dim`.

- `feature_extractor.cpp` (`get_fex_list_entry`): Unconditional call to
  `vmaf_dictionary_copy(&opts_dict, &slot->opts_dict)` when `opts_dict == NULL`
  returns `-EINVAL`, causing `vmaf_fex_ctx_pool_aquire` to fail for every
  no-options extractor context. The `.c` twin has `if (opts_dict != NULL)`
  guard; the `.cpp` mirror was missing it.

- `test_framesync.c`: Verification used the CURRENT frame's seed values
  (`ref[ctr] + dist[ctr] + 2`) to check the PREVIOUS frame's buffer, which
  was written with `seed_{N-1} + seed_{N-1} + 2 = 2*N`. The check produced
  `2*N+2` instead, failing at every non-zero index. Corrected to
  `(uint8_t)(2u * thread_data->index)`.

- `test_integer_motion_coverage.c`: Direct `vmaf_feature_extractor_context_extract`
  loop for the motion extractor did not set `ctx->fex->prev_ref` after each
  call. Production path (`vmaf_read_pictures`) does this automatically for
  `VMAF_FEATURE_EXTRACTOR_PREV_REF` extractors; the test must do so explicitly.

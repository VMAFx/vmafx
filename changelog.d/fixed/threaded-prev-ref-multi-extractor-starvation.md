- **Threaded extraction with two temporal extractors (e.g. `motion` +
  `motion_v2`) failed on every frame.** `threaded_read_pictures_batch`
  (`core/src/libvmaf.c`) took a single refcounted `prev_ref` snapshot,
  struct-copied it into the first `PREV_REF` extractor's thread-private
  context, and then zeroed the shared snapshot after that extractor's
  swap — starving every subsequent `PREV_REF` extractor, whose
  `extract()` then returned `-EINVAL` ("problem with feature extractor
  motion_v2") for all frames under `--threads N`. Since requesting
  `motion_v2` always co-schedules `motion`, **all** multi-threaded
  extractions involving `motion_v2` (e.g. the K150K corpus feature
  extraction) failed 100%; single-threaded runs were unaffected. Fix:
  give each `PREV_REF` extractor its own counted `vmaf_picture_ref()` and
  keep the shared snapshot live until the final unref. Regression test
  added in `test_thread_safety_batch.c`
  (`test_batch_two_prev_ref_extractors`). Scores are unchanged
  (threaded == single-threaded, verified on KoNViD-150K).

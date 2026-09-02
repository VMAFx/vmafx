### Fixed

- `test_pic_preallocation` ASan leak: `flush_context_threaded` called
  `fex->flush()` directly on the shared (never-initialized) extractor context
  in batch-threaded mode. For extractors like `integer_motion` that lazily
  allocate `s->feature_name_dict` in their flush path, the resulting
  allocation was never freed because `vmaf_feature_extractor_context_close`
  returned early when it found `is_initialized == false`. Fix: set
  `fex_ctx->is_initialized = true` before the flush loop so that teardown
  (`feature_extractor_vector_destroy`) correctly invokes `fex->close` and
  releases the dict. Detected by ASan `detect_leaks=1` in CI; passes all 8
  subtests under ASan + UBSan locally.

- **`feature_extractor.c` — restore NULL `opts_dict` semantics in
  `get_fex_list_entry` (PR #296 regression).** PR #296's tightened
  `vmaf_dictionary_copy` return-check landed without a `NULL`
  guard. Because `vmaf_dictionary_copy(NULL, …)` returns `-EINVAL`,
  every feature registered without options (the common case for
  `vmaf_use_feature(…, NULL)` and `vmaf_use_features_from_model()`)
  caused `vmaf_fex_ctx_pool_aquire` to return `NULL`. The first
  observed casualty was the master-CI ARM clang build (job
  78735195939) where `test_pic_preallocation`,
  `test_vif_skip_scale0_false`, and `test_feature_extractor` all
  failed at `vmaf_read_pictures` / `vmaf_fex_ctx_pool_aquire`; the
  bug is architecture-independent (reproduces locally on x86 clang)
  and would have failed every Build job once they dequeued. The
  fix wraps the copy in `if (opts_dict != NULL)`, matching the
  pre-#296 silent-no-op semantics that the rest of the file
  (`ctx_pool_ensure_slot_ctx`, `vmaf_use_feature`) already used.

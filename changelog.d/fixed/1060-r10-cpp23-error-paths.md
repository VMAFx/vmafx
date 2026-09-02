- **feature_extractor.cpp**: fix three-way memory leak in
  `vmaf_feature_extractor_context_create` when option parsing fails
  (`f->fex->priv`, `f->fex`, `f` are now freed before returning the error).
- **feature_extractor.cpp**: check `pthread_mutex_init` return in
  `vmaf_fex_ctx_pool_create`; add `free_fex_list` label so `fex_list` is also
  freed on failure (was leaked via the old `free_p` path).
- **feature_extractor.cpp**: check `pthread_cond_init` and
  `vmaf_dictionary_copy` returns in `get_fex_list_entry`; clean up the partial
  slot (`ctx_list` + cond) on failure instead of silently returning NULL.
- **feature_extractor.cpp**: propagate the first non-zero error code from
  `vmaf_feature_extractor_context_flush` in `vmaf_fex_ctx_pool_flush` instead
  of always returning 0.
- **read_json_model.cpp**: `model_parse` now checks `json_get_error` after the
  key loop and returns `-EINVAL` when the stream is in an error state, preventing
  a malformed JSON file from being accepted as valid after a successful
  `model_dict` parse (ADR-1060).

- **`integer_motion_v2`: `feature_name_dict` leaked in threaded `flush()` path
  (CWE-401, round-7 stability audit)** — In the threaded dispatch path,
  `flush()` is invoked on the *registered* `VmafFeatureExtractorContext` rather
  than on any pool instance. That context is never passed through
  `vmaf_feature_extractor_context_init`, so its `is_initialized` flag is `false`
  and `vmaf_feature_extractor_context_close()` returns early (line 536 guard)
  without calling `close_fex()`. When `s->feature_name_dict` is `NULL` at
  `flush()` entry (the registered-context path), `flush()` allocates a fresh
  dict but `close_fex()` is never called to free it, leaking 378 bytes (16 +
  128 + 117 + 117) per scoring run. Fix: track whether the dict was created
  locally within `flush()` via a `dict_locally_owned` flag and free it before
  returning when ownership was acquired. Confirmed via ASan with
  `--feature motion_v2` + all other CPU features; 0 bytes leaked after the
  fix across 100-frame and 50× init/destroy patterns.

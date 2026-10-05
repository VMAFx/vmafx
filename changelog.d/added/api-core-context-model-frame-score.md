- **VMAFx core API: contexts, models, host frames and scores (RC4, ADR-1852,
  ADR-1906).** A program can now score videos through `vmafx/*.h` alone:
  contexts with their own log callback (`VmafxContextConfig.log_callback`),
  context options (`vmafx_context_set_option`), feature option sets
  (`vmafx_options_set`), extractor, model and model-set registration
  (`vmafx_context_use_feature`, `vmafx_context_use_model`,
  `vmafx_context_use_model_set`, `vmafx_context_import_score`), feature
  resolution (`vmafx_feature_resolve`), refcounted models and model sets with
  the SHA-256 of the bytes as loaded (`vmafx_model_load`,
  `vmafx_model_load_file`, `vmafx_model_hash`, `vmafx_model_set_load`, ...),
  the CPU device (`vmafx_device_create`), host frames allocated or borrowed
  without a copy (`vmafx_frame_create_host`, `vmafx_frame_wrap_host`),
  submission (`vmafx_submit`, `vmafx_flush`), frame retention
  (`vmafx_context_frame_retention`) and synchronous per-frame and pooled
  scores for features, models and model sets (`vmafx_score_frame`,
  `vmafx_score_pooled`, `vmafx_feature_score_pooled`,
  `vmafx_score_frame_model_set`, `vmafx_score_pooled_model_set`), equal bit for
  bit to the `libvmaf.h` calls. One frame can be scored by several contexts
  without a copy. Errors also name what kind of subject failed and the
  function (`vmafx_error_subject_kind`, `vmafx_error_function`); an input
  struct below its introduction size is the new `VMAFX_E_ABI`. ABI 0.1.1. See
  [the VMAFx API page](docs/api/vmafx/index.md).

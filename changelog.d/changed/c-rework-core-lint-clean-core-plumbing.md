- Library-core plumbing (`core/src/libvmaf.c`, `core/src/predict.c`,
  `core/src/feature/feature_collector.c`, `core/src/read_json_model.cpp`) is
  now lint-clean to the fork's strictest clang-tidy profile (116 findings
  discharged) and to cppcheck: nine oversized
  functions were split into named helpers (`vmaf_init`, `vmaf_close`,
  `vmaf_read_pictures`, the threaded extractor batch, the tiny-AI attach /
  run-frame paths, the output writer, and `post_process_feature_from_another`),
  the repeated PREV_REF release and n_subsample skip idioms are shared helpers,
  and the C++ JSON model parser uses an anonymous namespace. Public C ABI,
  thread-safety contracts, and the Netflix golden scores are unchanged.
  Bootstrap-model score transforms now propagate a malformed knot-list error
  instead of silently continuing, and
  `vmaf_predict_score_at_index_model_collection` rejects NULL arguments with
  `-EINVAL`. C translation units keep `NULL` per ADR-1138 (`modernize-use-nullptr`
  stays a C++-only ratchet).

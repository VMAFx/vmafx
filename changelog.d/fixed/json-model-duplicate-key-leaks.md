- **Two memory leaks in the JSON model parser, both on duplicate-key inputs,
  found by the nightly `fuzz_json_model` LeakSanitizer lane.** The lane went red
  on master at `042c48adc7` with `248 byte(s) leaked in 6 allocation(s)`; both
  leaks are reproduced byte-for-byte by seeds now committed to the corpus.
  1. **A duplicate `model` key inside `model_dict` orphaned an entire
     `svm_model`.** `parse_libsvm_model` assigned `model->svm` unconditionally,
     so the second occurrence overwrote the first and nothing held a pointer to
     it any more — neither `vmaf_model_destroy` nor `svm_free_and_destroy_model`
     could reach it. That is the 184-byte direct leak plus its `sv_coef` array
     and rows. It now frees the previous model before overwriting, exactly as
     the duplicate `feature_names` key is already handled in
     `append_feature_name` (ADR-0887). Fixed in **both** twins:
     `core/src/read_json_model.cpp`, which the library builds, and
     `core/src/read_json_model.c`, which the fuzz harness compiles directly.
  2. **A duplicate header row inside the libsvm model text leaked its array.**
     `SVMModelParser::parse_header()` `Malloc`'d `rho`, `label`, `probA`,
     `probB` and `nSV` unconditionally, so a model text carrying e.g. two `rho`
     rows orphaned the first allocation (8 bytes at `nr_class == 2`). A repeated
     header row is malformed input, so all five now reject it with the parser's
     existing `exceptAssert`, rather than silently picking a winner.
  Neither leak is reachable from a well-formed model: both require a duplicate
  key, so normal scoring is unaffected and no score changes. Netflix golden gate
  unchanged.

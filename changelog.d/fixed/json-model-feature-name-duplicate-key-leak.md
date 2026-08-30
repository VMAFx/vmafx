Fix a memory leak in the JSON model parser's feature-name path
(`core/src/read_json_model.c` and its C++23 twin `read_json_model.cpp`). A
duplicate `feature_names` key re-runs `parse_feature_names` from index 0, so
`append_feature_name` strdup'd a new name over `feature[index].name` without
freeing the prior value — orphaning the first name. `vmaf_model_destroy` only
walks the *current* slot occupants, so the orphan leaked on both the
validation-error path (where `*model` is nulled and the caller correctly does
not destroy) and the success path. Found by the nightly `fuzz_json_model`
LeakSanitizer lane. Both parser variants now free any existing name before the
overwrite; added an ASan regression test
(`test_json_model_feature_names_duplicate_key_no_leak`) to `core/test/test_model.c`.

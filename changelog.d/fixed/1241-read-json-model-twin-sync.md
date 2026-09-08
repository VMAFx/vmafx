- **JSON model parser C and C++23 twins brought into full lockstep.**
  Fixed desynchronizations between `core/src/read_json_model.c` (compiled
  by `core/test/fuzz/meson.build` into `fuzz_json_model`) and its C++23 twin
  `core/src/read_json_model.cpp` (compiled into `read_json_model_cpp23_lib`
  for `libvmaf`):
  (1) `read_json_model.c` gains the ADR-1060 defect #5 stream-error check
  (`if (json_get_error(s)) return -EINVAL;` after the unknown-key skip loop in
  `model_parse`), preventing malformed JSON streams after `model_dict` from
  returning success in the fuzz harness.
  (2) `read_json_model.cpp` gains ADR-0887 cross-key per-feature length mismatch
  validation (`sync_n_features` in all walkers, `validate_feature_arrays` in
  `parse_model_dict`), rejecting malformed JSON models whose per-feature arrays
  disagree in length with `-EINVAL` at parse time.
  (3) Residual "lusoris vmaf fork" product names updated to "VMAFx fork" in
  `tools/vmaf-roi-score/README.md`, `tools/vmaf-tune/README.md`, and
  `docs/research/README.md`.

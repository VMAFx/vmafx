## Rust model prediction (RC4 lane P, #1723)

- `core/src/predict.c` splits `vmaf_predict_score_at_index()` into the score
  gather (unchanged), `predict_compute_c()` (the former body, statement for
  statement) and `predict_compute_rust()`; an upstream sync keeps the C body in
  `predict_compute_c()` and any arithmetic change to `normalize()`, `transform()`,
  `clip()`, `post_process_feature_from_another()`, `piecewise_*` or `svm_predict()`
  is mirrored in `core/src/rust/predict/src/` in the same PR
  (`scripts/ci/rust_twin_diff.py --models`). `struct VmafModel` carries three
  trailing fields (`rust_predict_state`, `rust_predict`, `predict_raw`).
- `predict.c` reaches Rust only through the `struct VmafRustPredictOps` table
  declared at the end of `core/src/predict.h`; `vmaf_model_destroy()`
  (`core/src/model_lifetime.c`) calls `vmaf_rust_predict_destroy()` and frees
  `predict_raw`. Keep both on a sync that touches those files. No score, public
  API or FFmpeg patch impact while `VMAF_FEATURE_IMPL` is unset.
- `core/src/rust/include/*.h` are cbindgen 0.29.4 output, committed byte for
  byte (`scripts/dev/rust-abi-header.sh`); the clang-format hooks and
  `make format` skip that directory. Regenerate them, never format or merge
  them by hand (`scripts/ci/tests/test_rust_abi_header_verbatim.py`).

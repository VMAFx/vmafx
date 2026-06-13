- Windows MSVC build: add `pthread_dependency` to `read_json_model_cpp23_lib`
  static library in `core/src/meson.build`; `read_json_model.cpp` includes
  `model.h` which carries `pthread_mutex_t predict_cache_lock` (added by
  PR #864). Same win32-shim rationale as `picture_pool_cpp23_lib` (PR #838).
- DNN test suite: treat `-EIO` from `vmaf_dnn_session_open` as a skip
  condition alongside `-ENOENT` in `test_dnn_session_api.c` and
  `test_ep_fp16.c`. ORT 1.22+ defers the dlopen of
  `libonnxruntime_providers_cuda.so` to `CreateSession`; on CPU-only CI
  runners the provider `.so` is absent and the CPU fallback can fail with
  `-EIO` when ORT internal state is damaged by the failed dlopen.
- CI workflow `e2e-k8s.yml`: replace unresolvable
  `docker/setup-buildx-action@b5ca514...` (bad SHA from PR #880) with
  the correct `v4.1.0` commit SHA `d7f5e7f509e...`. Same fix applied to
  `docker-publish-production.yml` and `docker-publish-operator-node.yml`.
- Assertion density (Power of 10 §5): add `assert()` entry-point guards
  to 19 fork-added SIMD hot-path functions across `arm64/` and `x86/` that
  had zero asserts in functions ≥ 20 lines. Assertions check non-null
  pointer and positive dimension preconditions. Gate now exits 0.
- Rust `vmafx` crate: fix doctest borrow-checker error in `lib.rs`; the
  example passed `&mut model` to both `use_features_from_model` and
  `score_pooled`, which conflicts. Changed both signatures to `&Model`
  (shared reference) since neither C call mutates the model's Rust-visible
  state. Doctest now declares `model` before `ctx` to satisfy drop order.

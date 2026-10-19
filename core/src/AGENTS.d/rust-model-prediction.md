---
paths:
  - core/src/predict.c
  - core/src/predict.h
  - core/src/predict_internal.h
  - core/src/model.h
  - core/src/model_lifetime.c
  - core/src/rust/predict/**
  - core/src/rust/shim/rust_predict.c
  - core/src/rust/shim/rust_predict.h
invariant: Rust predictor returns C predictor's score bit for bit; change to one changes other in same PR.
---
<!-- markdownlint-disable MD013 -->
# Rust model prediction (RC4 lane P, #1723)

`core/src/rust/predict/` (`vmafx-predict`) is `predict.c`'s post-gather
arithmetic, statement for statement: `normalize()`, `post_process_feature_from_another()`,
`svm_predict()` (EPSILON_SVR / NU_SVR with linear, polynomial and RBF
kernels), `denormalize()`, `transform()`, `piecewise_*()` and `clip()`.
`VMAF_FEATURE_IMPL=rust` selects it per process; C predictor stays default.

1. **Change to any of those C routines changes `core/src/rust/predict/src/` in
   same PR.** `core/test/test_rust_predict.c` (suite `rust`) compares both
   predictors bit for bit on every `vmaf_v1.0.16*` model and on edited copies;
   `scripts/ci/rust_twin_diff.py --models` does same end to end.
2. **`vmaf_predict_score_at_index()` keeps score gather in C**
   (`predict_load_feature_score()`: collector lock, `-EAGAIN`, name cache) and
   append of prediction; only `predict_compute_c()` / `predict_compute_rust()`
   differ. Sync keeps C body in `predict_compute_c()`.
3. **No silent fallback.** Model Rust predictor refuses (classification SVM,
   precomputed or sigmoid kernel, unknown normalisation) runs on C predictor
   and WARNING says so; table's `create` returns `-ENOTSUP` for it. With
   `VMAF_FEATURE_IMPL=rust` and no table installed (build without Rust) C
   predictor runs and WARNING says that too. Mode comes from
   `vmaf_feature_impl_rust_requested()`, framework's one reader of
   variable; value other than `c` or `rust` fails prediction with
   `-EINVAL`.
4. **Libm**: `exp` goes through `vmafx_fex::libm` (C's `exp`); never `f64::exp`,
   never `mul_add`; `-gamma * sum` is `(-gamma) * sum`.
5. Scratch of Rust call is C predictor's own `predict_nodes` (`struct
   svm_node` layout asserted in `rust_predict.c`); Rust predictor does not
   allocate per frame.
6. **Only libvmaf links Rust archive (ADR-1713).** `predict.c`, `model.c` and
   `model_lifetime.c` (`predict_c` archive and test binaries compiling them)
   reach Rust predictor only through `struct VmafRustPredictOps` table
   declared in `predict.h` and stored in `predict.c`
   (`vmaf_predict_install_rust_ops()`; NULL = C predictor).
   `core/src/rust/shim/rust_predict.c` defines table on `vmafx_rs_model_*` and
   `vmaf_rust_predict_install()`; it is listed in `rust_shim_sources`, and
   `vmaf_ctx_subsystems_init()` calls installer next to
   `vmaf_rust_twins_install()` under `#if HAVE_RUST_FEATURES`. No Rust symbol,
   no Rust include and no `HAVE_RUST_FEATURES` branch belongs in those three
   files. `vmaf_model_destroy()` frees handle through
   `vmaf_rust_predict_destroy()`. `core/test/test_predict_rust_ops.c` (every
   build, stand-in table) guards routing, `test_rust_predict` installation by
   `vmaf_init()`.

### Fixed

- **DNN tiny-AI / ONNX Runtime correctness (T-BUGHUNT-DNN-2026-06-27)**: three
  bugs in the DNN path fixed.
  - `f32_to_f16_one` (`core/src/dnn/tensor_io.c`) turned large finite floats
    that overflow the f16 range (e.g. `70000.0f`, `1e30f`) into NaN instead of
    `±inf` — the `exp >= 31` branch propagated the input mantissa
    unconditionally, so every overflowing finite value acquired a non-zero
    half-mantissa (= NaN). It now sets the NaN mantissa only when the f32 input
    is genuinely inf/NaN (biased exponent `== 0xff`); overflowing finite values
    map to a clean `±inf`, matching `ort_backend.c:fp32_to_fp16`.
  - `copy_output_tensor` (`core/src/dnn/ort_backend.c`) `memcpy`'d a non-float
    ORT output tensor as float, producing garbage scores for models with a
    DOUBLE/INT64/INT32 output. It now branches per element type (FLOAT,
    FLOAT16, DOUBLE, INT64, INT32) and returns `-ENOTSUP` for any other dtype.
  - `vmaf_ort_infer` omitted the positive-dimension and overflow validation
    that `vmaf_ort_run` already performed. The shared `build_input_tensor` now
    rejects empty rank, non-positive dims, and an element-count product that
    overflows `size_t` (`-EINVAL` / `-EOVERFLOW`), guarding both call paths.
  - Regression tests: `test_f16_finite_overflow_to_inf`
    (`core/test/dnn/test_tensor_io.c`) and `test_ort_infer_rejects_bad_shape`
    (`core/test/dnn/test_ort_internals.c`). Golden-safe: only the DNN tiny-AI
    surface is touched, not the VMAF metric engine; no Netflix golden
    assertion changes.
- **DNN NCHW input-shape `int` narrowing guard (round-2 finding R2-7)**:
  `dnn_attach_nchw` (`core/src/libvmaf.c`) and the luma fast-path in
  `vmaf_dnn_session_open` (`core/src/dnn/dnn_api.c`) narrowed the ONNX
  spatial dims (`int64_t`) to `int` (`expected_w`/`expected_h`, `s->w`/`s->h`)
  with only a positivity check — an untrusted export carrying `H`/`W > INT_MAX`
  would silently truncate into a wrong expected geometry. Both now reject
  dims `> 32768` (mirroring the picture-dimension cap `VMAF_PIC_DIM_MAX`)
  before the narrowing: `dnn_attach_nchw` returns `-ENOTSUP`; the fast-path
  condition falls through to the generic `vmaf_dnn_session_run()` path (no
  truncation). CERT INT31-C. Compile-verified (libvmaf.c builds CPU-only; the
  gated `dnn_api.c` path is covered by the CI `+ DNN` build); a defensive
  bound that does not change any in-range model's behaviour.

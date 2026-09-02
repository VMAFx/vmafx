## Added

- **`core/test/dnn/test_dnn_session_api.c`**: three new tests pushing
  coverage on `core/src/dnn/dnn_api.c` toward the ADR-0114 90 % per-file
  floor — `test_session_open_oversize_sidecar_returns_error` exercises
  the non-`ENOENT` sidecar-load error branch (lines 78-79: free + return
  when `vmaf_dnn_sidecar_load` returns `-EFBIG` for a > 1 MiB JSON);
  `test_session_open_symbolic_batch_skips_luma_fast_path` and
  `test_session_symbolic_batch_run_plane16_returns_notsup` drive the
  standalone-session path for a model whose input has a symbolic batch
  dim (ADR-0523), confirming both `run_luma8` and `run_plane16` return
  `-ENOTSUP` when the legacy luma fast-path scratch buffers were not
  allocated.
- **`core/test/dnn/test_tensor_io.c`**: `test_f16_special_values` extended
  with two true fp16-subnormal-range inputs (`1.0e-6f`, `2.0e-5f`) so the
  rounding branch (lines 33-35 of `f32_to_f16_one`) is now executed; the
  original `1e-8f` annotation claimed to cover that branch but actually
  trips the flush-to-zero path. `core/src/dnn/tensor_io.c` is now at
  100 % line coverage.

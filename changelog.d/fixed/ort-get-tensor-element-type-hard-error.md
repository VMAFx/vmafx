- **`GetTensorElementType` errors during session open were silently discarded,
  leaving `input_elem_types[i]` / `output_elem_types[i]` at
  `ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED` (0).** The run path then silently
  emitted fp32 tensors regardless of the model's declared type, meaning a
  malformed or mistyped ONNX model would appear to load successfully but
  produce incorrectly-typed tensor inputs. The `ort_discard_status()` wrapper
  calls on `GetTensorElementType` have been replaced with a checked path that
  releases the OrtStatus, releases the OrtTypeInfo, logs a `WARNING` via
  `vmaf_log`, and returns `-EINVAL` — making the failure a hard error at open
  time. Two regression-lock tests added to `test_ort_internals`:
  `test_ort_open_elem_types_populated` (fp32 smoke model must populate FLOAT,
  not UNDEFINED) and `test_ort_open_elem_types_fp16_model` (fp16 model must
  populate FLOAT16). Found by PR #112 ORT audit.

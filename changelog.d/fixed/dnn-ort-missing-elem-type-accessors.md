- Restore `vmaf_ort_internal_input_elem_type` and
  `vmaf_ort_internal_output_elem_type` accessor functions to
  `ort_backend_internal.h` and both branches of `ort_backend.c`
  (real-ORT and stub) that were accidentally removed by PR #515.
  Add the `VmafOrtElemType` enum (`ELEM_TYPE_UNDEFINED`, `ELEM_TYPE_FLOAT`,
  `ELEM_TYPE_FLOAT16`) to the internal header so `test_ort_internals.c`
  compiles again. Fixes the DNN test suite build breakage introduced
  in the C memory-safety bundle.

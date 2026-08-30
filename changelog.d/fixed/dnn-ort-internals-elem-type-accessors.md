### Fixed

- **DNN / ORT internals**: add missing `vmaf_ort_internal_input_elem_type` and
  `vmaf_ort_internal_output_elem_type` accessor functions and `VmafOrtElemType`
  enum to `ort_backend_internal.h` / `ort_backend.c`. Their absence caused a
  hard build failure in `test/dnn/test_ort_internals.c`, which blocked the
  Netflix CPU Golden Tests (D24) CI job on every master push. Both the
  real-ORT (`VMAF_HAVE_DNN`) and stub (`!VMAF_HAVE_DNN`) paths are covered so
  the test binary links on all build configurations.

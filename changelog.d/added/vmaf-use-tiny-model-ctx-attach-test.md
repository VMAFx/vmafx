- **`core/test/dnn/test_vmaf_use_tiny_model.c`**: 5 new unit tests covering
  the `vmaf_use_tiny_model()` public ctx-attach entry point, which had zero
  C-unit-test coverage (identified in `audit-test-coverage-2026-05-16.md §2`).
  Covers: null-ctx rejection, null-path rejection, non-existent path error,
  `-ENOSYS` stub contract on disabled DNN builds, and the `smoke_v0.onnx`
  happy path (when DNN is enabled and the fixture is present). Registered under
  `suite : ['dnn', 'fast']`.

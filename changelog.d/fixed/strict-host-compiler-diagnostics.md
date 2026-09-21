- Eliminate the host-compiler diagnostics in the feature-context growth path,
  the model and SSIMULACRA2 tests and integer PSNR. The capacity bound keeps
  both its count guard and its byte guard while comparing same-width values
  (the old form was provably false on a 64-bit host), aggregates and option
  sentinels initialise completely, and the full-range BT.709 arm of the
  SSIMULACRA2 scalar reference exits explicitly instead of falling through. No
  warning was suppressed and no lint, HISS, golden-score or tolerance baseline
  changed.
- Fix `vmaf_feature_extractor_context_create()` leaking the extractor's private
  state when a context was rejected for an unknown option, and stop the two
  feature-extractor pool diagnostics calling the concurrency-mt-unsafe
  `strerror()` from the threaded path.
- Split the CUDA SpEED chroma extractor by resource class and retire its
  cleanup jumps and function-size exceptions, with launch order, stream
  synchronization, eigendecomposition and QR order, singular flags and U/V
  aggregation unchanged. A failed CUDA context pop is retried once before its
  original error is returned, and a failed host-plane download now reports the
  driver errno instead of a flat `-EIO`.
- Stop `test_vmaf_cuda_gpumask` flaking against its 10-second budget. The budget
  is now 120 seconds, sized from measurement (9.70 s worst case against a ~1 s
  steady state), and the test no longer shares the GPU with
  `test_vmaf_cuda_threads`. Its 1920x1080 fixture is unchanged: measurement
  shows the fixture is not what spends the budget.

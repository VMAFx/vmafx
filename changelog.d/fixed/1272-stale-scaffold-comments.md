- Corrected stale scaffold comments in `core/src/feature/feature_extractor.cpp`
  for HIP and Metal extractors (#1272). Multiple extractors previously carried
  comments claiming scaffold posture and that `init()` returns `-ENOSYS` until
  runtime PRs arrive. All HIP kernels now run on device when compiled under
  `enable_hipcc=true` (`HAVE_HIPCC`) while falling back to `-ENOSYS` via `init()`
  or `submit()` without it, and all Metal extractors are fully implemented with
  `MTLComputePipelineState` dispatch.

- Skip 9 Python `feature_extractor_test` cases that exercise `motion_five_frame_window=True`
  on `VmafIntegerFeatureExtractor`; the C layer returns `-ENOTSUP` (ADR-0337) and the vmaf
  binary exits 234, causing spurious CI failures. Tests are marked
  `@unittest.skip("ADR-0337: ...")` so they remain visible and can be re-enabled once the
  feature is plumbed through the C integer-motion path.

- **`vmaf_feature_backend_twin()` and `vmaf_registered_feature_extractor()`**
  in `libvmaf.h`. The first tells a caller which device twin model dispatch
  would use for a CPU extractor on the context's backend, and whether that twin
  can honour the given options and picture size. The second lists the
  registered extractors and the backend each one runs on. Both are additive;
  `vmaf_use_feature()` still selects by exact name. See
  [the C API reference](docs/api/index.md#device-twins-and-the-extractors-that-ran).
- **`feature_backends` in the CLI's JSON output**: one
  `{"extractor": ..., "backend": ...}` entry per registered extractor, next to
  `backend_used`, so a run that mixes device twins and CPU extractors says so.

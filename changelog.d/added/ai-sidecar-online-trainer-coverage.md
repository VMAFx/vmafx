### test(ai): coverage push — online_trainer and feature_extractor unit tests

Added two new test files lifting two previously untested modules from 0% to
high coverage:

- `ai/sidecar/tests/test_online_trainer.py` (30 tests) — covers
  `OnlineTrainer`, `_build_fallback_model`, `_load_base_model`,
  `_write_sha256_sidecar`, `_handle_connection` protocol layer, and
  `run_server` start/stop lifecycle. Runs CPU-only; requires PyTorch.
- `ai/tests/test_feature_extractor_unit.py` (26 tests) — covers
  `_ensure_binary`, `_lookup` (direct key + `integer_` fallback),
  `_run_vmaf_json` argv composition and temp-file cleanup,
  `extract_features` happy/NaN/empty-frames paths, and
  `aggregate_clip_stats` statistics + error paths.

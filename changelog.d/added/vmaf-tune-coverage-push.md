## Added

- **`tools/vmaf-tune/tests/test_coverage_push_lowcov_modules.py`**: focused unit
  tests (92 cases) targeting the lowest-covered vmaftune modules. Coverage
  on the package now climbs from 87% to 88% with no production changes; the
  largest per-module gains are `uncertainty.py` (70% to 100%),
  `_gop_common.py` (78% to 100%), `predictor_features.py` (56% to 82%),
  `encoder_profile.py` (76% to 93%), and `benchmark.py` (86% to 95%). All
  added tests are pure unit-level: no subprocess spawn, no ffmpeg, no ONNX
  runtime, no GPU.

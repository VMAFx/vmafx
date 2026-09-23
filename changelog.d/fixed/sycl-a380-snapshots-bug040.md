- `testdata/run_sycl_scores.py`: fix backend selection to explicitly pass
  `--backend sycl` instead of relying on `--no_cuda` (BUG-040, ADR-0214). Pin execution
  environment to Intel GPUs (`ONEAPI_DEVICE_SELECTOR=level_zero:gpu`).
- `testdata/test_run_sycl_scores.py`: add unit test regression suite for
  `testdata/run_sycl_scores.py` verifying `--backend sycl`, prohibition of `--no_cuda`,
  device targeting, environment setup, and score comparison.
- `testdata/generate.sh`: add `-movflags frag_keyframe+empty_moov` to the intermediate
  MP4 pipe to resolve FFmpeg n9.0.1 stdout stream seekability errors when deriving
  distorted YUV test fixtures.
- `testdata/scores_sycl_a380_*.json` + `testdata/scores_cpu_*.json`: regenerate all five
  Arc A380 SYCL snapshots (576, 640, 720, 1080, 4k) and matching CPU snapshots in the
  same pass. Resolves the zero-motion anomaly (`integer_motion` and `integer_motion2`
  were 0.0 across all 48 frames) and constant VMAF 97.428043 degeneracy at 576/640,
  incorporating 162 SYCL commits and achieving cross-backend parity ($\le 0.000165$
  max per-frame delta vs CPU across all resolutions).

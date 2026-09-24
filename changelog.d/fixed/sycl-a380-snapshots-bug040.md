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
  incorporating 162 SYCL commits and achieving cross-backend parity ($\le 0.000091$
  max per-frame delta vs CPU across all resolutions).
- `core/src/libvmaf.c` (picture cleanup paths): fix host-buffer DMA lifetime
  race causing production-path integer_adm2/VMAF nondeterminism at 4K on Intel Arc A380.
  The picture pool (`tools/vmaf.cpp`, 3 slots) recycled `dist` back to the reader thread
  while `copy_queue.memcpy` DMA was still reading from that host buffer, corrupting
  in-flight GPU data. Added `vmaf_sycl_wait_last_upload(vmaf->sycl.state)` to both serial
  and `--threads` cleanup paths under `#ifdef HAVE_SYCL` to drain the last DMA event
  without blocking GPU compute on `combined_queue`. Verified with 20 consecutive 4K runs
  per path on Arc A380: vmaf=90.747255 adm2=1.013590 (bit-exact, vs CPU delta 1.6e-05 ≤
  tolerance). `testdata/test_sycl_4k_repeat_determinism.py` binds the selected binary to
  its exact library and compares the full normalized report; it skips gracefully without
  4K YUV fixtures.

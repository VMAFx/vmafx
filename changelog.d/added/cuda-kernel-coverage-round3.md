- **CUDA kernel parity coverage — round 3 (ADR-0947)** — five new
  `core/test/test_cuda_*_parity.c` files asserting CPU-vs-CUDA score
  agreement at `places=4` / 1e-4 (ADR-0214 cross-backend gate) for the
  remaining float-path twins and the new ssimulacra2 kernel:
  - `test_cuda_float_psnr_parity` — `float_psnr_cuda` vs. `float_psnr`,
    probes the scalar `float_psnr` feature
  - `test_cuda_float_vif_parity` — `float_vif_cuda` vs. `float_vif`,
    probes all four `VMAF_feature_vif_scale[0..3]_score` features
  - `test_cuda_float_ms_ssim_parity` — `float_ms_ssim_cuda` vs.
    `float_ms_ssim`, probes the scalar `float_ms_ssim` feature
  - `test_cuda_float_moment_parity` — `float_moment_cuda` vs.
    `float_moment`, probes all four `float_moment_{ref,dis}{1st,2nd}`
    features
  - `test_cuda_ssimulacra2_parity` — `ssimulacra2_cuda` vs. `ssimulacra2`,
    probes the scalar `ssimulacra2` feature on the newly-landed kernel
  Each test allocates a 256x144 YUV420P 8-bpc synthetic fixture, runs 3
  frames through both backends, reads the score at frame index 1, and
  skips cleanly with `[skip: no CUDA device]` when `vmaf_cuda_state_init`
  fails (CPU-only CI lanes). Mirrors the `test_cuda_motion3_parity.c`
  template. Wired into `core/test/meson.build` behind the
  `enable_cuda` guard, suite `['fast', 'gpu']`. Builds on PR #351
  (round 1: psnr_cuda + ciede_cuda) and PR #374 (round 2: adm/
  motion_v2/cambi/psnr_hvs/integer_ssim). Combined post-merge CUDA-
  extractor parity coverage rises to ~72 % (13 of 18 kernels); the
  remaining `speed_chroma_cuda`, `speed_temporal_cuda`, and
  `float_motion_cuda` are deferred to a follow-up ADR with their own
  tolerance budget (host-side eigendecomp + motion blend overlap).

- **CUDA kernel parity coverage — round 4 (ADR-0956)** — five new
  `core/test/test_cuda_*` files closing the last gap in CUDA-extractor
  cross-backend coverage. After this PR every CUDA feature extractor
  on the fork (19 of 19) has a fork-local gate that fires in
  `--suite=fast` / `--suite=gpu`:
  - `test_cuda_float_adm_parity` — `float_adm_cuda` vs. `float_adm`
    CPU twin; asserts `places=4` / 1e-4 agreement (ADR-0214) on the
    `VMAF_feature_adm2_score` aggregate plus all four
    `adm_scale[0..3]_score` sub-scores
  - `test_cuda_float_motion_parity` — `float_motion_cuda` vs.
    `float_motion` CPU twin; asserts agreement on the two features
    both backends emit (`motion`, `motion2`; motion3 is host-side
    and already covered by `test_cuda_motion3_parity.c`)
  - `test_cuda_float_ssim_parity` — `float_ssim_cuda` vs. `float_ssim`
    CPU twin; asserts agreement on the scalar `float_ssim` feature
  - `test_cuda_speed_chroma_smoke` — CUDA-only smoke gate for
    `speed_chroma_cuda` (no CPU twin emits
    `Speed_chroma_feature_*_score`); asserts finite output on all
    three channels (u, v, uv) over a 640x360 fixture sized to admit
    a non-singular covariance matrix in the ADR-0567 host-side
    eigendecomp path
  - `test_cuda_speed_temporal_smoke` — CUDA-only smoke gate for
    `speed_temporal_cuda`; asserts finite `speed_temporal` output
    over the same 640x360 fixture
  All five wire into `core/test/meson.build` behind the `enable_cuda`
  guard, suite `['fast', 'gpu']`, and emit `[skip: no CUDA device]`
  when `vmaf_cuda_state_init` fails (CPU-only CI lanes). Builds on
  PRs #351 (round 1: psnr/ciede), #374 (round 2: adm/motion_v2/
  cambi/psnr_hvs/integer_ssim), #442 (round 3: float-path twins +
  ssimulacra2). Combined post-merge coverage reaches 100 %
  (19 of 19 CUDA kernels gated); the kernel-coverage backlog is
  closed for CUDA. HIP / SYCL / Metal coverage tracked separately.

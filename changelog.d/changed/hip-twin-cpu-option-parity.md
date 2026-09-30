- **Four HIP twins take the CPU extractor's options (ADR-1382).** `psnr_hip`
  now accepts `enable_mse`, `enable_apsnr`, `reduced_hbd_peak` and `min_sse`
  through the CPU's own `core/src/feature/psnr_score.h`, `apsnr_*` aggregates
  included; `integer_ssim_hip` accepts `enable_db` / `clip_db`,
  `float_ssim_hip` `enable_lcs` / `enable_db` / `clip_db` (`float_ssim_l/c/s`
  computed on the device), and `float_motion_hip` `motion_max_val`, with its
  debug `motion` score now weighted by `motion_fps_weight` like the CPU's.
  Before, a model that set one of these options computed the feature on the
  CPU, and naming the twin with the option failed with `unknown option`.
  `float_ssim_hip` now scores each pixel as the CPU does (`l * c * s` from the
  CPU's luminance, contrast and structure terms) and rounds the frame mean to
  fp32 like the CPU, so with `enable_db` identical frames report what the CPU
  reports (72.247 dB on a flat frame, where the CPU's fp32 arithmetic leaves
  1 - 2^-24) instead of a forced `+inf`; `integer_ssim_hip` scores identical
  frames from 3x3 up exactly 1, as the CPU does. `motion_v2_hip` now stores
  its SAD weighted by `motion_fps_weight` and capped at `motion_max_val` like
  the CPU (it weighted at fold time and never capped) and scores one-frame
  runs; `psnr_hip` now sees every frame under `--subsample`, so `apsnr_*`
  covers the whole clip; `motion_hip` defaults `debug` to false and emits
  `VMAF_integer_feature_motion_sad_score`, as the CPU `motion` does. The
  parity gate (`scripts/ci/cross_backend_parity_gate.py`) takes `--backends
  hip` and a `float_ssim_lcs` cell. On a gfx1036 `psnr_hip` with all four
  options matches the CPU exactly, `apsnr_*` included, and the parity gate
  passes every HIP cell; see
  [the HIP backend guide](docs/backends/hip/overview.md#measured-on-a-gfx1036-2026-10-01).

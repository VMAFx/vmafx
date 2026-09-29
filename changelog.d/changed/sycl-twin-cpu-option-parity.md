- **Four SYCL twins take the CPU extractor's options and match it
  (ADR-1365).** `psnr_sycl` now accepts `enable_mse`, `enable_apsnr`,
  `reduced_hbd_peak` and `min_sse` and matches `--backend cpu` bit for bit,
  `apsnr_*` aggregates included; the PSNR option math now lives in
  `core/src/feature/psnr_score.h`, shared by the CPU extractor and the twin.
  `integer_ssim_sycl` accepts `enable_db` / `clip_db`, `float_ssim_sycl`
  `enable_lcs` / `enable_db` / `clip_db` (`float_ssim_l/c/s` within 8.3e-7 of
  the CPU), and `float_motion_sycl` `motion_max_val`. Before, a model or a
  `--backend sycl --feature psnr=enable_mse=true`-style request that set one of
  these options computed the feature on the CPU, and naming the twin with the
  option failed with `unknown option`. Both SSIM twins now score an
  identical window exactly 1, so with `enable_db` identical frames report the
  CPU's `+inf` or `clip_db` ceiling; their default linear scores moved by at
  most 1.1e-8. Measured on an Arc B580 and a UHD 770; see
  [the SYCL backend guide](docs/backends/sycl/overview.md#cpu-options-on-the-psnr-ssim-and-float-motion-twins-2026-09-29).

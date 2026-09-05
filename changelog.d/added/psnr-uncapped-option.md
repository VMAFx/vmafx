- `psnr` and `float_psnr` gained an opt-in `uncapped` option (bool,
  default `false`) that stops the `psnr_max` ceiling from truncating
  genuinely computed scores, while keeping it as the `mse == 0`
  infinity sentinel. The ceiling used to serve both roles at once, so
  an 8-bit 576x324 pair differing by a single luma step reported
  `psnr_y = 60.000000` where the true value — and FFmpeg's own `psnr`
  filter — is `100.840479` (Netflix/vmaf#1109,
  `T-UPSTREAM-1109-PSNR-CAP-TRUNCATES-2026-09-03`). The option is
  mirrored under the same name and default on all eight GPU twins
  (CUDA / SYCL / HIP / Metal, integer and float). Default behaviour is
  bit-identical to previous releases; the Netflix golden 60 / 84 /
  108 dB assertions pin `sse == 0` pairs and are untouched. New
  `docs/metrics/psnr.md` documents both `uncapped` and the
  pre-existing, previously undocumented `min_sse` escape hatch. See
  [ADR-1193](docs/adr/1193-psnr-uncapped-option.md).

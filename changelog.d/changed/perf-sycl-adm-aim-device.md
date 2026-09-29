- **The SYCL integer ADM twin computes AIM on the device, so the default model's
  ADM no longer falls back to the CPU under `--backend sycl` (ADR-1362).**
  `adm_sycl` now emits `VMAF_integer_feature_aim_score` and
  `VMAF_integer_feature_adm3_score`, and every ADM output (adm2 and the four
  scales included) is now bit for bit equal to `--backend cpu` on
  8-bit and 10-bit input, odd and 17x17 frames, with the default and the
  default model's options. With the default model on an Arc B580, a 3840x2160
  frame drops from 48.4 to 9.2 ms at the default `--threads 0` and from 24.3 to
  9.7 ms at `--threads 16` (CPU backend with 16 threads: 27-32 ms). On a UHD 770
  the same run gets slower (4K: 75.7 to 87.5 ms, and 40.5 to 79.4 ms at
  `--threads 16`), because the iGPU now does the ADM work the CPU used to do
  beside it. The twin also accepts
  `adm_skip_aim`. Before, adm2 and the scales were up to 2.9e-7 from the CPU
  (double host finalisation) and, on 4K content, `integer_adm_scale2` up to
  1.40e-6 (a decouple ratio that wrapped at scales 1-3).
  See [SYCL backend](docs/backends/sycl/overview.md).

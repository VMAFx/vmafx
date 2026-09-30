- **SYCL: `psnr_hvs_sycl` kernel is scratch-free on Intel Arc under xe (ADR-1395).**
  On the Linux `xe` driver, private memory causes corrupted reads and writes.
  The kernel had 2432 B/thread of private memory at SIMD16 on DG2 (Arc A380)
  due to dynamically indexed `means[4]` and `variances[4]` arrays in
  `hvs_variance_ratio()` and `args.plane[plane]` dynamic indexing in `hvs_locate()`,
  causing ~20 dB divergence at 4K (BBB frame 0 `psnr_hvs_y` 13.13 dB vs CPU
  33.17 dB). Replacing dynamic struct indexing with explicit member branches
  and restructuring quadrant accumulators into scalar members (`HvsQuadrants`)
  eliminates all private memory and register spills (`private_size: 0`,
  `spill: 0`). On Arc A380 under `xe`, scores match CPU reference across 576x324
  (max diff 8.37e-5 dB vs 5e-4 gate), 1080p (max diff 1.71e-3 dB), and 4K BBB
  (frame 0 33.161817 dB vs CPU 33.171624 dB, delta 0.0098 dB; max diff across
  22 frames 1.099e-2 dB). Throughput on 4K BBB improves from 12.55 ms/frame
  (corrupted) to 10.90 ms/frame (correct) (`T-SYCL-PSNR-HVS-XE-SCRATCH-2026-09-30`).

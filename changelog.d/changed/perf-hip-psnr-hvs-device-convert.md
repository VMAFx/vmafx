- **HIP `psnr_hvs` uploads native samples and converts on device (ADR-1369 port).**
  `integer_psnr_hvs_hip` uploads raw native samples via `vmaf_hip_picture_upload()`
  (uint8_t for 8 bpc, uint16_t for 9–12 bpc) and converts them to integers on the
  device in `psnr_hvs_score.hip`. This eliminates host float conversion loops and
  removes 6 unused pinned host staging allocations (`h_uint_ref` and `h_uint_dist`).
  On AMD gfx1036, 4K batch throughput improves by 44% (3.44 to 4.95 FPS, 221.79 ms/frame).
  Output scores are bit-identical to the baseline twin on 576x324 and 4K BBB (max abs diff 0.0),
  and within 8.37e-05 dB of CPU reference at 576x324. Also resolves a latent scaling defect
  on 9-bit and 11-bit depths, now validated by `test_psnr_hvs_deep_parity`.

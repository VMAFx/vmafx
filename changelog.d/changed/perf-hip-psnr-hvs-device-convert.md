- **HIP `psnr_hvs` uploads native samples and converts on device (ADR-1369 port).**
  `integer_psnr_hvs_hip` uploads raw native samples via `vmaf_hip_picture_upload()`
  (uint8_t for 8 bpc, uint16_t for 9–12 bpc), converts them to integers on the
  device in `psnr_hvs_score.hip`, and fuses all plane dispatches into a single
  kernel (`n_dispatches_per_frame = 1`). This eliminates host float conversion loops
  and removes 6 unused pinned host staging allocations (`h_uint_ref` and `h_uint_dist`).
  On AMD gfx1036, 4K frame time drops from 221.79 ms to 18.90 ms/frame (CPU 16t is 6.04 ms/frame).
  Output scores are within 8.37e-05 dB of CPU reference at 576x324 and within area-scaled
  tolerance at 4K. Also resolves a latent scaling defect on 9-bit and 11-bit depths,
  validated by `test_psnr_hvs_deep_parity`.

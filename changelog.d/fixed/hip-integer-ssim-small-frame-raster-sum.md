- **`integer_ssim_hip` scores frames of up to 4096 pixels exactly as the CPU
  `ssim` does (ADR-1400).** The CPU adds one term per pixel in raster order,
  and on an identical frame the result is 1 or an ulp or two below it,
  depending on the frame: with `enable_db` an identical 1x1 frame of zeros
  reports 156.54 dB and a flat 3x3 frame of 51 reports 159.55 dB. The HIP twin
  reduced per block and reported `+inf` for every identical frame. For frames
  of at most 64x64 pixels the device now writes one term per pixel and the
  host adds them in the CPU's order, so the score equals the CPU's bit for
  bit at 8, 10, 12 and 16 bits, identical frames or not. Larger frames are
  unchanged. Verified on a gfx1036 by `test_hip_ssim_tiny_frames`.

- **`psnr_hvs_hip`, `ssimulacra2_hip` and `float_ms_ssim_hip` read device
  frames on the device (ADR-2092).** Given a frame in HIP device memory, the
  three twins that staged planes on the host copy or convert them on the
  device (`float_ms_ssim_hip` builds level 0 with a kernel of the same
  arithmetic as `picture_copy()`); host frames are read as before.

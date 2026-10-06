- **The CUDA MS-SSIM twin no longer copies frames through the host.**
  `float_ms_ssim_cuda` converted every plane of every frame on the host
  (a device-to-host copy, a wait, `picture_copy()` and an upload); the
  conversion now runs on the device with the same arithmetic, so its scores
  are unchanged and the frame stays on the GPU.

- **`float_ansnr_hip`: `hipMemcpy2DAsync` direction tagged
  `hipMemcpyDeviceToDevice` for host→device transfer**: `submit_fex_hip`
  in `core/src/feature/hip/float_ansnr_hip.c:324,330` copied
  host-side `ref_pic->data[0]` / `dist_pic->data[0]` into device-side
  staging buffers `s->ref_in` / `s->dis_in` but tagged the transfer
  direction as `hipMemcpyDeviceToDevice`. Modern ROCm tolerates this
  (auto-detects from pointer attributes) but the wrong tag is
  undefined per the HIP spec and inconsistent with every other HIP
  feature kernel in the fork (`float_psnr`, `integer_psnr`,
  `float_moment`, `float_ssim`, `float_motion` all correctly use
  `hipMemcpyHostToDevice`). Surfaced during the round-6 ROCm/HIP bench
  audit on `gfx1036` (post-PR-#710). Fix: two
  `hipMemcpyDeviceToDevice` → `hipMemcpyHostToDevice` corrections.

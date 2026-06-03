# perf(cuda): psnr_hvs — `__ldg()` + `__launch_bounds__(64)` (ADR-0764)

Apply the F3 `__ldg()` / `__restrict__` pointer-extraction pattern (established
in ADR-0754 for `calculate_ssim_vert_combine`, PR #93) to the `psnr_hvs` CUDA
kernel (PR #96 candidate #5):

- Extract `const float *__restrict__ ref_buf` and `const float *__restrict__
  dist_buf` from the `VmafCudaBuffer` struct arguments before the cooperative
  tile load.
- Use `__ldg()` on both element reads, routing them through the L1 read-only
  texture cache (LDG.E.CONSTANT).
- Add `__launch_bounds__(64)` matching the actual 8x8 block launch.

Predicted -3 to -5% kernel duration at 1080p. Bit-identical scores (ADR-0214
places=4). No API or ABI change.

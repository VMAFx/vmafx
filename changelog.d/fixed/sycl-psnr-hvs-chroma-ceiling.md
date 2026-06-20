- **SYCL `integer_psnr_hvs` chroma plane geometry on odd-dimension frames**:
  `integer_psnr_hvs_sycl.cpp::init_fex_sycl` derived the 4:2:0 / 4:2:2 chroma
  plane width/height with floor division (`w >> 1` / `h >> 1`), but `picture.c`,
  the CPU reference, and the CUDA and HIP twins all use ceiling division
  (`(w + 1U) >> 1`). On odd-width or odd-height inputs the SYCL path dropped the
  last chroma 8x8 block strip, producing `psnr_hvs_cb` / `psnr_hvs_cr` /
  `psnr_hvs` scores that silently diverged from every other backend. The fix
  switches both chroma dimensions to ceiling division, restoring cross-backend
  parity. Even-dimension inputs were unaffected. Found by the full-fork
  correctness audit.

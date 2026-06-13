- **SYCL PSNR chroma plane geometry on odd-dimension YUV420**: `integer_psnr_sycl.cpp`
  now uses ceiling division `(w + 1U) >> 1` / `(h + 1U) >> 1` instead of truncating
  `w / 2U` / `h / 2U` for the chroma plane width/height, matching CPU, CUDA, and (since
  PR #878) Vulkan behaviour. Prevents a places=4 cross-backend parity failure on
  odd-width or odd-height YUV420 inputs.

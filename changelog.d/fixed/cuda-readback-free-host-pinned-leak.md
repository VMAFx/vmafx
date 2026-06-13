**Fix:** `vmaf_cuda_kernel_readback_free` now calls `vmaf_cuda_buffer_host_free`
to release the pinned host readback buffer. Previously the helper set
`rb->host_pinned = NULL` without calling `cuMemFreeHost`, silently leaking one
`cuMemHostAlloc` allocation per init/close cycle for every feature extractor that
uses the kernel-template readback pair: `integer_psnr_cuda`, `integer_ssim_cuda`
(float), `ssim_cuda` (integer), `float_psnr_cuda`, `float_motion_cuda`,
`integer_ciede_cuda`, `integer_moment_cuda`, `integer_motion_v2_cuda`, and
`integer_cambi_cuda`. PR #93 follow-up sweep (2026-05-29).

**HIP ms_ssim: add `picture_copy()` uint→float normalization** —
`ms_ssim_hip_upload_plane()` (introduced in commit `681ab99451`) was
calling `hipMemcpy2DAsync` with `dpitch = width * bpc_bytes` directly
into a `width * height * sizeof(float)` device buffer.  This wrote only
`width*height` raw uint8 bytes, leaving the remaining three quarters
uninitialized, with no uint→float conversion.  The decimate and horiz
kernels consequently read garbage values.  The fix mirrors the CUDA path
in `integer_ms_ssim_cuda.c`: two pinned host staging buffers (`h_ref`,
`h_cmp`) are allocated via `hipHostMalloc`, `picture_copy()` converts
uint samples → float [0, 255] into them, and a single `hipMemcpyAsync`
uploads the contiguous float plane to the device.  The parity test
`test_hip_ms_ssim_parity` (originally written in the
`gpu-runtime-bug-audit` worktree) is wired into `core/test/meson.build`
under `suite=['fast','gpu']` to gate CPU vs. HIP agreement at
`places=3` (1e-3) per ADR-0883.

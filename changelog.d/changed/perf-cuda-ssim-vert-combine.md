# perf(cuda): SSIM vert_combine — __ldg() + __launch_bounds__ + pinned-host leak fix

Route all 55 inner-loop loads in `calculate_ssim_vert_combine` through the
read-only L1 texture cache via `__ldg()` (extracts `const float *__restrict__`
pointers from the 5 `VmafCudaBuffer` struct arguments before the inner loop).
Adds `__launch_bounds__(128)` to constrain register allocation to the actual
128-thread launch configuration (mirrors ADR-0743 VIF filter1d pattern).

Fixes a pinned-host memory leak in `close_fex_cuda`: `vmaf_cuda_buffer_host_free`
was never called for `rb.host_pinned` after `vmaf_cuda_kernel_readback_free`
(which NULLs the pointer without freeing it), leaking one page of CUDA pinned
host memory per `vmaf_close()` cycle.

ADR: [ADR-0754](../docs/adr/0754-cuda-ssim-vert-combine-ldg-pinned-leak.md)

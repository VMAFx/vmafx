## Added: Runtime resolution-aware CUDA kernel variant dispatch (ADR-0753)

A new `vmaf_cuda_workload_class(w, h)` classifier (`core/src/feature/cuda/resolution_dispatch.{h,c}`)
maps each frame's luma pixel count to `WS_SMALL` (< 720p), `WS_MEDIUM` (720p–4K), or
`WS_LARGE` (>= 4K) at runtime. Feature extractors use this to pick the optimal kernel
variant without requiring separate binaries.

Three kernels now dispatch at runtime:

1. `adm_cm_line_kernel_8` / `_no_bounds` (`integer_adm_cuda.c::adm_cm_device`) — selects
   `__launch_bounds__(128,8)` at WS_MEDIUM for the −9.3% 1080p kernel-time saving; falls
   back to the no-bounds variant at WS_SMALL / WS_LARGE.

2. `filter1d_8_horizontal_kernel_2_17_9` / `_no_bounds` (`integer_vif_cuda.c::filter1d_8`)
   — selects the bounded variant at WS_MEDIUM + WS_LARGE (occupancy from 75% to 83.3% on
   sm_89); uses the no-bounds variant at WS_SMALL where the kernel is wave-limited and the
   hint provides no gain.

3. `calculate_ssim_vert_combine` / `_no_bounds` (`integer_ssim_cuda.c::submit_fex_cuda`)
   — same WS_MEDIUM + WS_LARGE / WS_SMALL policy; `__ldg()` loads retained in both variants.

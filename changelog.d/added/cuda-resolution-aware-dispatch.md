### Added: Runtime resolution-aware CUDA kernel variant dispatch (ADR-0753)

A new `vmaf_cuda_workload_class(w, h)` classifier (`core/src/feature/cuda/resolution_dispatch.{h,c}`)
maps each frame's luma pixel count to `WS_SMALL` (< 720p), `WS_MEDIUM` (720p–4K), or
`WS_LARGE` (>= 4K) at runtime. Feature extractors use this to pick the optimal kernel
variant without requiring separate binaries.

First consumer: `integer_adm_cuda.c::adm_cm_device()` selects
`adm_cm_line_kernel_8` (with `__launch_bounds__(128,8)`) at WS_MEDIUM and
`adm_cm_line_kernel_8_no_bounds` at WS_SMALL / WS_LARGE, recovering the
−9.3% kernel-time saving at 1080p without regressing 576p or 4K.

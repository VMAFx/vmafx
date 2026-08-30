### CUDA adm_cm `__launch_bounds__` confirmed -9.3% at 1080p (Research-0749 / ADR-0750)

Hardware measurement on RTX 4090 (CUDA 13.3, ncu 2026.2.0) confirms the
`adm_cm_line_kernel_8 __launch_bounds__(128, 8)` optimization from branch
`perf/cuda-ms-ssim-decimate-adm-cm-ncu-driven-20260528`: register file
reduction 114→64 regs/thread (-43.9%) delivers -9.3% kernel duration at
1080p. `ms_ssim_decimate` smem tiling is measured as a regression (+8-24%
kernel duration; baseline L1 hit rate already 95%) and is recommended for
revert. End-to-end CUDA throughput: +4.8% WL1 (576p), +3.9% WL2 (1080p).

### CUDA ADM/motion/SSIM/MS-SSIM ncu hotpath profiles published

Per-kernel ncu `--set basic` profiles collected on RTX 4090 (sm_89, CUDA 13.3) for the
four remaining CUDA metric families. Digests at Research-0734 (ADM), Research-0735
(motion), Research-0736 (SSIM), Research-0737 (MS-SSIM), and Research-0738 (cross-metric
summary). Top finding: all ADM and MS-SSIM kernels are launch-starved at 576×324
(< 1 wave across 128 SMs); motion achieves ~62% occupancy; SSIM vert_combine is
DRAM-bound at 55.8%. Three highest-leverage candidates identified: (1) fix missing
`extern "C"` in `integer_ssim_score.cu` (P0 correctness — int64 SSIM CUDA path crashes
at runtime), (2) shared-memory tile staging for `ms_ssim_decimate` (est. +30–40% at
1080p+), (3) register reduction in `adm_cm_line_kernel_8` to lift theoretical occupancy
from 33% to 75%.

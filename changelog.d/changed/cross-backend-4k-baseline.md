<!--
  Copyright 2026 Lusoris
  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
-->
### Cross-backend 4K (3840×2160) baseline + PR #79 adm_cm A/B at 4K

Research-0751 establishes the first measured 4K CUDA throughput baseline on RTX 4090
(BigBuckBunny 3840×2160, 8-bit yuv420p, 24 frames, `vmaf_bench`):

- vif CUDA: **147 fps** (7.0× over CPU 21 fps)
- adm CUDA: **161 fps** (2.3× over CPU 69 fps)
- motion CUDA: **176 fps** (0.6× over CPU 291 fps — launch-overhead-dominated at
  24 frames)

Key findings:

- `filter1d_8_horizontal_kernel_2_17_9` (PR #76 target): fully saturated at 4K
  (32400 CTAs / 128 SMs = 253 waves, 69.7% active warps). The 0.84-wave
  launch-width-limit seen at 576p is eliminated. PR #76 optimization is fully
  expressed at production 4K resolutions.
- `adm_cm_line_kernel_8` (PR #79 `__launch_bounds__`): the -9.3% kernel speedup
  from the 1080p measurement (Research-0749) does not extend to 4K (−0.3%, within
  noise). At 4K the kernel runs 32 waves and the scheduler is no longer
  register-bound. The optimization remains beneficial for 576p–1080p.
- `ms_ssim_decimate` at scale 0 (4K): 88.1% active warps, 126 waves — fully
  saturated. The smem tiling revert from Research-0749 is confirmed correct at 4K;
  the kernel was already L1-resident (>99.5% hit rate).

Profile `core/src/feature/cuda/integer_vif/filter1d.cu` with Nsight
Compute 2026.2.0.0 on RTX 4090 (sm_89). Primary hotspot:
`filter1d_8_horizontal_kernel_2_17_9` (35 % of VIF filter time).
Diagnosis: launch-width-limited (0.84 waves / 128 SMs), register
pressure (56 regs/thread → 75 % theoretical occupancy), L2 imbalance
(+46 % hot-slice). Report: `docs/research/0734-cuda-vif-filter1d-ncu-hotpath-20260528.md`.

### CUDA VIF filter1d 1080p re-measurement (research-0748)

PR #76 (`perf/cuda-vif-filter1d-ncu-driven-20260528`) validated at production
1080p workload: `filter1d_8_horizontal_kernel_2_17_9` shows +6.85 pp active
warps, +3.6% end-to-end fps (checkerboard 1080p median), and confirmed `__ldg`
L1-texture routing (+54.7% l1tex traffic). Register reduction 56→48 confirmed
by ncu. Correctness delta: 0.000000 (bit-identical on checkerboard 1080p pair).
ADR-0743, research-0748.

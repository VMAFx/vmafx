## CUDA VIF filter1d — NCU A/B measurement published (Research-0857)

Live ncu measurements confirm the `__launch_bounds__(128, 10)` + `__ldg()` optimization
for `filter1d_8_horizontal_kernel_2_17_9` (ADR-0743): registers 56 → 48, warp activity
+6.0 pp at 1080p, l1tex traffic +54.7% (`__ldg` routing confirmed), E2E fps +1.8% at
1080p on RTX 4090 sm_89. Bit-identical correctness. Research-0857.

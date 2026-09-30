- **SYCL integer ADM row reduction runs spill-free on DG2 and restores Arc A380 parity under `xe`.**
  In `integer_adm_sycl.cpp`, `launch_csf_den_cm` kept nine 64-bit accumulators live across the
  column loop, which IGC compiled at SIMD16 with an 864 B/thread register spill on DG2 (Arc A380).
  Under the Linux `xe` driver, scratch memory accesses return corrupted values, causing all ADM
  accumulators to evaluate to zero and failing `test_sycl_adm_parity` (`integer_adm3_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02`
  reported CPU 0.5 vs SYCL 1.0). The kernel is restructured into two sequential column reduction
  phases (CSF denominator 3 sums, then DLM and AIM contrast measures 6 sums) with sub-group partials
  staged in local memory before folding, completely eliminating private and spill memory (`privateMemSize: 0`,
  `spillMemSize: 0` on Arc A380). All 7 parity cases in `test_sycl_adm_parity` and all 6 in
  `test_sycl_adm_tiny_frames` pass, and `speed_gpu_parity.py --backend sycl --feature adm` is bit-exact
  (0.000e+00 delta) against CPU on both 576x324 (48/48 frames) and 3840x2160 (50/50 frames).
  Throughput at 4K on BBB 3840x2160 8-bit is 10.92 ms/frame vs 10.70 ms/frame before
  (ADR-1395, `T-SYCL-ADM-CM-SCRATCH-2026-09-30`).

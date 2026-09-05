- Reverted PR #865 pseudo-Kahan recurrence in `core/src/feature/sycl/ssimulacra2_sycl.cpp`
  and calibrated Intel Arc A380 tolerance (`T-SYCL-ARC-SSIMULACRA2-PARITY-2026-06-03`,
  ADR-0985). Commit 8b7ae731a incorrectly added `prev1` to the output of the Charalampidis
  3-pole recursive blur filter, shifting poles outside the unit circle ($1 - d1 \approx -0.8422$)
  and causing exponential oscillation to > 10^{25}, NaN, and saturation at 100.0. The filter
  is restored to pure float32 recurrence matching the CUDA twin. Calibrated Intel Arc A380
  (`sycl:0x8086:0x56a*` and `arc:dg2-g10`) in `scripts/ci/gpu_ulp_calibration.yaml` to measured
  places=1 tolerance `5.0e-2` (Option C per Research-0985 §4.4), reflecting fp64-less accumulation
  across the 6-scale pyramid on natural video clips. Hardware validation on Arc A380 confirms
  all parity gates pass cleanly (`test_sycl_ssimulacra2_parity` delta=4.98e-5 < 5e-3; 48-frame
  `src01` delta=1.21e-2 <= 5.0e-2).

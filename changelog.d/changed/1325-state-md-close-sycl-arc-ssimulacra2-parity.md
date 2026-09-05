- **docs(state):** Close `T-SYCL-ARC-SSIMULACRA2-PARITY-2026-06-03` in
  `docs/state.md`. The defect the row named — uncompensated fp32
  accumulation in the 3-pole IIR Charalampidis blur of
  `core/src/feature/sycl/ssimulacra2_sycl.cpp::launch_blur` — was fixed on
  2026-06-12 by `8b7ae731a` (PR #865), which added Kahan
  compensated-summation state to all three poles, satisfying the row's own
  closure condition (b). The ledger text was written against `dd02029f9`
  (PR #852, the calibration-anchor partial fix) and was never refreshed, so
  the row read as open for three months. The stale comment block and
  `notes:` prose for the `sycl:0x8086:0x56a*` entry in
  `scripts/ci/gpu_ulp_calibration.yaml` are corrected the same way; the
  entry stays `status: placeholder` and its `ssimulacra2: 5.0e-3` tolerance
  is unchanged, because the outstanding item is an Arc A380 re-measurement,
  not a code fix. No behaviour change.

- **SYCL / Arc A380 (DG2-G10) ssimulacra2 calibration placeholder**
  (`scripts/ci/gpu_ulp_calibration.yaml`): add a named calibration entry
  (`sycl:0x8086:0x56a*`, label "Intel Arc Alchemist DG2/G10") for the
  `ssimulacra2` feature, anchored at the default places=2 (5e-3) tolerance.
  The entry ensures the cross-backend parity gate matches Arc A380 explicitly
  rather than falling through to the coarse `sycl:0x8086:*` generic glob.
  Root cause of the observed 8.72e-2 divergence: fp32-IIR accumulation drift
  in the 3-pole Charalampidis blur (`ssimulacra2_sycl.cpp::launch_blur`);
  Arc A380 lacks native fp64 (ADR-0220). The placeholder value does not yet
  cover the observed divergence — a hardware-validated tolerance replacement
  and a Kahan-compensated IIR rewrite are tracked as follow-ups in
  `docs/state.md` (T-SYCL-ARC-SSIMULACRA2-PARITY-2026-06-03).

HIP kernel parity-test coverage round 3: 4 new tests
(`test_hip_cambi_parity`, `test_hip_float_adm_parity`,
`test_hip_float_motion_parity`, `test_hip_float_psnr_parity`) close
the next batch after PR #351 (round 1) and PR #372 (round 2).  Lifts
HIP coverage from 9/17 to 13/17 parity-gated extractors (≈76%).
Tolerances follow ADR-0214 (places=4 unfiltered, places=3 filtered).
See ADR-0945.

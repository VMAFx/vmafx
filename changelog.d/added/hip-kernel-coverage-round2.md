HIP kernel parity-test coverage round 2: 5 new tests
(`test_hip_ciede_parity`, `test_hip_psnr_hvs_parity`,
`test_hip_motion_parity`, `test_hip_ssim_parity`,
`test_hip_ms_ssim_parity`) close the cross-backend parity gaps left
after PR #351 (round 1).  Lifts HIP coverage from 4/17 to 9/17
parity-gated extractors (≈53%).  Tolerances follow ADR-0214
(places=4 unfiltered, places=3 filtered).  See ADR-0883.

SYCL kernel parity-test coverage round 2: five new CPU-vs-SYCL parity
gates (`test_sycl_adm_parity`, `test_sycl_ciede_parity`,
`test_sycl_ssim_parity`, `test_sycl_ms_ssim_parity`,
`test_sycl_motion_v2_parity`) at ADR-0214 places=4 tolerance. Closes
the round-1 follow-up backlog (ADR-0868 / PR #351 covered
`psnr_sycl` + `integer_vif_sycl`). See ADR-0884.

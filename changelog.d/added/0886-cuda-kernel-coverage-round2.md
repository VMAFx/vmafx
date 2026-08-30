CUDA kernel parity-test coverage round 2: 5 new tests
(`test_cuda_adm_parity`, `test_cuda_motion_v2_parity`,
`test_cuda_cambi_parity`, `test_cuda_psnr_hvs_parity`,
`test_cuda_ssim_parity`) close the highest-impact gaps not covered
by ADR-0868 / PR #351 — every kernel feeding the libvmaf-2.x.x
default model lineage (ADM, motion_v2, CAMBI, PSNR-HVS, SSIM) is
now under a places=4 cross-backend gate per ADR-0214. Raises CUDA
extractor assertion coverage from ~25 % to ~53 %. See ADR-0886.

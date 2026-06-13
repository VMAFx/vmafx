GPU backend kernel parity-test coverage gap-fill: 7 new tests
(`test_cuda_psnr_parity`, `test_cuda_ciede_parity`,
`test_hip_psnr_parity`, `test_hip_vif_parity`,
`test_sycl_psnr_parity`, `test_sycl_vif_parity`,
`test_metal_kernel_registration`) close cross-backend parity gaps for
PSNR/CIEDE/VIF kernels and audit the 8-extractor Metal registration
surface. Tolerances follow ADR-0214 (places=4 unfiltered,
places=3 filtered). See ADR-0868.

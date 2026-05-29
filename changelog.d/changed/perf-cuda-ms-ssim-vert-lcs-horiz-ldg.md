# perf(cuda): MS-SSIM `ms_ssim_vert_lcs` + `ms_ssim_horiz` — `__ldg()` + `__launch_bounds__` (F3 fix #2, ADR-0757)

Route all inner-loop loads in `ms_ssim_vert_lcs` (5×11 = 55 loads) and
`ms_ssim_horiz` (2×11 = 22 loads) through the read-only L1 texture cache via
`__ldg()`. Extracts `const float *__restrict__` pointers from each `VmafCudaBuffer`
argument before the inner loop, making the alias-free invariant visible to the
compiler so it can emit `LDG.E.CONSTANT` (confirmed in sm_89 SASS). Adds
`__launch_bounds__(128)` to both kernels to constrain register allocation to the
actual 128-thread launch configuration.

Second application of the F3 pattern established in ADR-0754 for
`calculate_ssim_vert_combine`. Predicted −4 to −6% kernel duration at 1080p
(memory-bound regime where combined intermediate footprint exceeds L2 capacity).

ADR: [ADR-0757](../docs/adr/0757-cuda-ms-ssim-vert-lcs-horiz-ldg.md)

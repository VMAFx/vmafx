### perf(float-adm): wire AdmSimdDispatch — AVX2/AVX-512/NEON kernels now called

The four float-ADM SIMD kernels (`dwt2`, `csf`, `csf_den_scale`, `sum_cube`)
were compiled for AVX2, AVX-512, and NEON but the dispatch table was never
initialised, so every `compute_adm()` call fell through to the scalar path.
`adm_prime_simd_dispatch()` is now called from `float_adm.c` `init()`,
activating the SIMD paths at extractor startup. Expect significant CPU speedup
on x86_64 and aarch64 hosts with `float_adm` enabled.

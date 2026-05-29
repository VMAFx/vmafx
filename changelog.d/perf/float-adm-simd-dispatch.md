Wire float_adm AVX2/AVX-512/NEON kernels to dispatch table (PR #116 F1).

The four float-ADM SIMD kernels (dwt2, csf, csf_den_scale, sum_cube) were
compiled for all ISAs but never called — every compute_adm() call fell through
to the scalar path regardless of CPU capabilities. A static AdmSimdDispatch
table in adm_tools.c now routes to the fastest available ISA path, primed
once at extractor init time via adm_prime_simd_dispatch().

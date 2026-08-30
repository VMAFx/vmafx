### perf(x86): AVX-512 path for float_moment

Add `moment_avx512.c` / `moment_avx512.h` — a 16-lane ZMM widening of
the existing AVX2 8-lane `float_moment` kernels.  On CPUs exposing
`VMAF_X86_CPU_FLAG_AVX512` the dispatch in `float_moment.c` now selects
the wider path at runtime, halving the number of inner-loop iterations
for HD/UHD frames.  Four AVX-512 parity cases added to
`test_moment_simd`; ADR-0987.

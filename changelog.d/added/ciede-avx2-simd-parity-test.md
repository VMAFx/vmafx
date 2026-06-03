### test(simd): bit-exact parity test for ciede AVX2 preprocessing kernels

Added `core/test/test_ciede_simd_parity.c` — a new `suite: ['fast', 'simd']`
test that verifies `ciede_preprocess_8_avx2` and `ciede_preprocess_16_avx2`
produce bit-identical float output to their scalar equivalents for both 8-bit
and 16-bit YUV inputs (including the scalar tail path for widths not divisible
by 8). Wired into `core/test/meson.build` under `x86_64`/`x86` arch gates.

**Fix build failure in Cppcheck CI gate caused by missing AVX-512 motion sub-kernel functions.**

`test_motion_avx512_parity.c` called `sad_avx512`, `y_convolution_8_avx512`,
`y_convolution_16_avx512`, and `x_convolution_16_avx512`, which were never
implemented in `motion_avx512.c`.  The compiler emitted implicit-function-declaration
errors that caused `meson compile -C build` to exit with code 1, preventing
cppcheck from running at all.

The fix adds AVX-512 implementations of these four sub-kernels:

- `sad_avx512` — luma-plane pixel-wise SAD between two `VmafPicture`s
- `y_convolution_8_avx512` — vertical 5-tap Gaussian on 8-bit source
- `y_convolution_16_avx512` — vertical 5-tap Gaussian on 16-bit source
- `x_convolution_16_avx512` — horizontal 5-tap Gaussian on 16-bit source

All four functions are declared in `motion_avx512.h` and use scalar fallback paths
for edge pixels, with AVX-512 SIMD for interior pixels.

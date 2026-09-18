- **CAMBI uses AVX-512 and NEON for every stage AVX2 accelerates**: the
  anti-dithering filter, derivative row, decimate, mode filter and c-values
  stage now have AVX-512 kernels, and all but the mode filter have NEON kernels
  in use (for the mode filter and the mask row the compilers already vectorise
  the scalar code on aarch64). Several of these kernels had been built but not
  called since the upstream CAMBI optimisation batch was ported. The AVX-512
  and NEON c-values stage skips the pixels that leave its sliding histogram
  unchanged, which is most of them on the flat content CAMBI looks at. On a
  Zen 5 core a whole CAMBI frame runs 1.23–1.32x faster than before with GCC
  and about twice as fast as the AVX2 path with Clang or icx; under
  `qemu-aarch64` the NEON stages execute 9–44 % of the scalar instructions.
  Scores are byte-identical on every path. In Clang and icx builds the AVX2
  c-values stage is still slower than scalar on CPUs without AVX-512; see
  [CAMBI CPU SIMD paths](docs/metrics/cambi.md#cpu-simd-paths) and
  [Research-2065](docs/research/2065-cambi-simd-gaps.md).

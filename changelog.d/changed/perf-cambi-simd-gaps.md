- **CAMBI's SIMD paths cover every stage and skip the work that changes
  nothing**: the anti-dithering filter, derivative row, decimate, mode filter
  and c-values stage now have AVX-512 kernels, and all but the mode filter have
  NEON kernels in use (for the mode filter and the mask row the compilers
  already vectorise the scalar code on aarch64). Several of these kernels had
  been built but not called since the upstream CAMBI optimisation batch was
  ported. The c-values stage on AVX2, AVX-512 and NEON now skips the pixels
  that leave its sliding histogram unchanged, which is most of them on the flat
  content CAMBI looks at; upstream's AVX2 c-values driver, which was slower
  than the scalar code in icx builds (icx builds the published container), is
  no longer used. On a Zen 5 core a whole CAMBI frame runs 1.23–1.32x faster
  than before with GCC at the default AVX-512 dispatch, and a CPU without
  AVX-512 gets 1.21–1.78x depending on the compiler; under `qemu-aarch64` the
  NEON stages execute 9–44 % of the scalar instructions. Scores are
  byte-identical on every path. See
  [CAMBI CPU SIMD paths](docs/metrics/cambi.md#cpu-simd-paths) and
  [Research-2065](docs/research/2065-cambi-simd-gaps.md).

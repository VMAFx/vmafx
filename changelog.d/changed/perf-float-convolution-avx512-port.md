### Performance

- **Float VIF convolution: AVX-512F port** (`core/src/feature/common/convolution_avx512.c`, ADR-0504)
  The separable float convolution inner loops (`vif_filter1d_s`, `_sq_s`, `_xy_s`) now
  prefer a 16-wide `_mm512_fmadd_ps` path on CPUs that expose AVX-512F, doubling the FMA
  width versus the AVX2 (8-wide) path. Expected +40–50 % throughput on the float VIF path
  (~60 % of float model wall time) on Skylake-X, Ice Lake, Sapphire Rapids, Zen 4, and
  later. Falls back transparently to AVX2 or scalar on non-AVX-512 hardware. The AVX-512
  path is gated behind `enable_avx512=true` in `meson_options.txt`.

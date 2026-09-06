- `compute_psnr()` accumulated `diff * diff` in `float` before widening to its
  `double` accumulator (CodeQL `cpp/integer-multiplication-cast-to-long`,
  alert 1009). The multiply now happens in `double`. No score moves — the
  function has no call sites and no SIMD twin, and all three Netflix golden
  pairs are bit-identical at `--precision=max`. The sibling alert on
  `iqa_convolve` (1005) is deliberately **not** fixed: its float multiply is the
  bit-exactness contract AVX2 / AVX-512 / NEON mirror under ADR-0138, and
  widening it fails `test_iqa_convolve`. See
  [research digest 2031](docs/research/2031-codeql-float-widening-multiplication.md).

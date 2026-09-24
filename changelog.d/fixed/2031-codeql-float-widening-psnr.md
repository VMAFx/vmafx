- `compute_psnr()` accumulated `diff * diff` in `float` before widening to its
  `double` accumulator (CodeQL `cpp/integer-multiplication-cast-to-long`,
  alert 1009). The multiply now happens in `double`. No score moves — the
  function has no call sites and no SIMD twin, and all three Netflix golden
  pairs are bit-identical at `--precision=max`. The sibling alert on
  `iqa_convolve` (1005) keeps its load-bearing float multiply: AVX2 / AVX-512 /
  NEON mirror that arithmetic under ADR-0138, and pre-widening it fails
  `test_iqa_convolve`. The complete SSIM/MS-SSIM/PU21 production domain now has
  executable bounds proving the flagged product stays at or below `2^28`, and
  the exact false-positive expression carries a narrow CodeQL source
  suppression. See
  [research digest 2031](docs/research/2031-codeql-float-widening-multiplication.md).

- **The AVX2 and AVX-512 integer ADM paths now match the scalar path on
  high-contrast content.** Part of scale 0's masking threshold is computed in
  16 bits and wraps on very large values in the scalar path, as it does in
  upstream Netflix/vmaf's C code; the SIMD paths kept 32 bits there. Content
  that reaches those values, such as full-range noise or sharp synthetic
  ramps, scored up to 7e-4 differently on AVX2/AVX-512 than on the scalar
  path. The SIMD paths now wrap the same way. Scalar scores do not change,
  and neither do SIMD scores on the Netflix reference clips, which never reach
  the wrap.

- **Integer ADM no longer shifts a negative masking threshold on the scalar
  path.** The scale-0 contrast-masking step computed
  `abs(x) - (threshold << shift)` in signed arithmetic, and the threshold is
  negative when one large coefficient stands among small ones. That shift is
  undefined in C: a sanitizer build of `vmaf --feature adm --cpumask 4294967295`
  stopped on full-range noise with `left shift of negative value`. The scalar
  path, which every aarch64 run uses, now computes the expression modulo 2^32,
  as the AVX2 and AVX-512 vector code already did. Scores are unchanged on
  every dispatch level ([features](docs/metrics/features.md)).

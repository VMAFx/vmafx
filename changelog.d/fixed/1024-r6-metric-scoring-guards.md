- **PSNR**: guard `log10(0)` NaN in APSNR flush for identical-frame sequences
  and when `enable_chroma=false`; fix theoretical cap inflation (`* 2` removed)
  (ADR-1024, `r6-psnr-correctness`)
- **PSNR SIMD**: fix signed integer overflow UB in 16-bit scalar tail of
  AVX2 / AVX512 / NEON paths — `int32_t e * e` overflows for max-diff 16-bit
  samples; cast to `uint32_t` before squaring (ADR-1024, `r6-psnr-correctness`)
- **ADM**: initialise `*score_aim = 1.0f` in the `den==0` (flat-frame) branch
  of `adm.c` and `integer_adm.c`; previously left uninitialised, emitting
  garbage AIM scores on black frames (ADR-1024, `r6-vif-adm-correctness`)
- **ADM**: guard harmonic-mean denominator in `float_adm.c` — when both
  `score` and `score_aim` are zero the `0/0` NaN is now replaced with `0.0`
  (ADR-1024, `r6-vif-adm-correctness`)

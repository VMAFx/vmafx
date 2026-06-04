# ADR-1024: R6 per-metric scoring guards — PSNR/ADM correctness fixes

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `correctness`, `psnr`, `adm`, `simd`

## Context

Round-6 static analysis identified several correctness bugs in the CPU metric
engine that produce NaN, infinite, or garbage scores under edge-case inputs:

1. **APSNR flush — `log10(0)` on zero SSE**: when all frames in a sequence are
   identical, `apsnr.sse[i]` accumulates to zero; `log10(0)` returns `-inf`
   and the aggregate score becomes NaN.  Additionally, when
   `enable_chroma=false` the flush loop still iterated over all three planes,
   calling `log10(0)` for the two unaccumulated chroma channels.  A secondary
   bug inflated the theoretical APSNR cap by ~1 unit via an unexplained `* 2`
   factor in the `max_apsnr` expression.

2. **PSNR 16-bit scalar tail — signed overflow UB in AVX2/AVX512/NEON**:
   `const int32_t e = ref - dis; result += (uint64_t)((uint32_t)(e * e))` —
   the multiplication `e * e` is evaluated in `int32_t` before the cast.
   Maximum 16-bit difference is 65535; 65535² = 4,294,836,225 > INT32_MAX,
   which is signed integer overflow (undefined behaviour in C11).  On
   UBSan builds this traps; on production builds compilers that assume
   no-overflow can miscompile the expression.

3. **ADM `score_aim` uninitialised on `den==0` path**: both `adm.c` and
   `integer_adm.c` set `*score` = 1.0 in the flat-frame branch but never
   write `*score_aim`; callers in `float_adm.c` and `integer_adm.c` read
   `score_aim` unconditionally, yielding garbage AIM scores on black frames.

4. **ADM harmonic-mean NaN**: `float_adm.c` computes
   `2 * score * score_aim / (score + score_aim)` without guarding the
   denominator; when both are zero (fully suppressed distortion) the result
   is `0/0 = NaN`, which `MAX(NaN, adm_min_val)` propagates silently.

## Decision

Apply targeted one-liner guards for each case:

- APSNR flush: bound the loop to `enable_chroma ? 3 : 1`; skip channels
  where `sse[i] == 0` and emit `psnr_max[i]` instead; remove the `* 2`
  inflation in `max_apsnr`.
- PSNR scalar tail: change `int32_t e` to `uint32_t e` (capturing the
  two's-complement bit-pattern of the signed difference) before squaring;
  the product is then `uint32_t * uint32_t` which cannot overflow unsigned.
- ADM `den==0` branch: add `*score_aim = 1.0f` in both `adm.c` and
  `integer_adm.c`.
- ADM harmonic mean: guard with `(score + score_aim == 0.0) ? 0.0 : ...`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Clamp SSE to 1 for APSNR | Single expression | Changes semantics of the zero-SSE clamped output value | Emitting psnr_max[i] is more intuitive |
| `(int64_t)e * e` for SIMD tail | Correct, no mask | Widens accumulator type unnecessarily | uint32_t is sufficient and matches the SIMD path semantics |

## Consequences

- **Positive**: APSNR no longer emits NaN for identical-frame sequences; ADM
  AIM scores are always valid; PSNR 16-bit scalar tail is UBSan-clean.
- **Negative**: APSNR cap value changes by ~1 unit for sequences where the
  old `* 2` cap was the binding constraint (negligible practical impact;
  cap is only hit on nearly-perfect sequences).
- **Neutral**: No change to the Netflix golden-data assertions (these bugs
  only manifest on edge inputs not present in the golden test pairs).

## References

- Round-6 scanner labels: `r6-psnr-correctness`, `r6-vif-adm-correctness`
- `core/src/feature/integer_psnr.c`, `x86/psnr_avx2.c`, `x86/psnr_avx512.c`,
  `arm64/psnr_neon.c`, `adm.c`, `integer_adm.c`, `float_adm.c`

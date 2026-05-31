# Research-0012: SSIM SIMD bit-exactness to scalar — where the ULP drifted

- **Status**: Active
- **Workstream**: [ADR-0139](../adr/0139-ssim-simd-bitexact-double.md)
- **Last updated**: 2026-04-21

## Question

The fork's AVX2 + AVX-512 `ssim_accumulate_*` (fork-local, commit
`81fcd42e`) were tightened in PR #18 (`f082cfd3`) with the stated
goal of being bit-identical to the scalar reference. A CLI XML diff
at `--precision max` on Netflix and checkerboard pairs surfaced a
residual **~0.13 float-ULP** divergence on `float_ms_ssim`
(8th decimal) between scalar and SIMD. `float_ssim`,
`ssim_precompute_*` and `ssim_variance_*` were already bit-exact;
the drift is localised to `ssim_accumulate_*`. Where does the ULP
come from, and what is the minimal rewrite to drive it to zero
without giving up SIMD?

## Sources

- Scalar reference:
  [`ssim_accumulate_default_scalar` in `ssim_tools.c`](../../core/src/feature/iqa/ssim_tools.c)
  (lines 174–205).
- Prior SIMD:
  [`ssim_accumulate_avx2`](../../core/src/feature/x86/ssim_avx2.c) /
  [`ssim_accumulate_avx512`](../../core/src/feature/x86/ssim_avx512.c)
  as of commit `f28d853e`.
- Upstream baseline: Netflix/vmaf `origin/master` has **no** SSIM
  SIMD at all — AVX2 / AVX-512 for VIF / ADM / motion / CAMBI only.
  `git log upstream/master -- core/src/feature/x86/ssim_*` → empty.
  First SSIM SIMD touch is fork commit `81fcd42e`.
- Measurement:
  - `vmaf --cpumask 255 ...` (all SIMD blocked — scalar path)
  - `vmaf --cpumask 16 ...`  (block AVX-512 bit; AVX2 active)
  - `vmaf`                   (native — AVX-512 active)
  - `--precision max` forces `%.17g` IEEE-754 round-trip printing.
- Relevant ADRs: [ADR-0138](../adr/0138-iqa-convolve-avx2-bitexact-double.md)
  (companion convolve fix),
  [ADR-0125](../adr/0125-ms-ssim-decimate-simd.md) (bit-exactness
  ground rule for SIMD paths),
  [CLAUDE.md §8](../../CLAUDE.md#8-netflix-golden-data-gate-do-not-modify)
  (Netflix golden gate).

## Findings

### Measured drift

| Fixture (1920×1080 checkerboard, frame 0) | Scalar | SIMD (AVX2 == AVX-512) | Δ |
| --- | --- | --- | --- |
| `float_ssim` | `0.93335253000259399` | `0.93335253000259399` | 0 |
| `float_ms_ssim` f0 | `0.89452544363435227` | `0.89452545888605872` | ≈ 1.525e-8 |
| `float_ms_ssim` f1 | `0.93540067693522488` | `0.93540069286261140` | ≈ 1.593e-8 |

`float_ssim` is bit-identical; `float_ms_ssim` drifts ~1.5e-8 (about
0.13 float-ULP on a value near 1.0). The drift is present on AVX2
*and* AVX-512 identically — so it's a pattern problem, not a
16-lane-vs-8-lane reduction issue.

### Why precompute / variance were already fine

```c
// ssim_precompute_*_scalar
ref_sq[i]  = ref[i] * ref[i];
cmp_sq[i]  = cmp[i] * cmp[i];
ref_cmp[i] = ref[i] * cmp[i];

// ssim_variance_*_scalar
ref_sigma_sqd[i] -= ref_mu[i] * ref_mu[i];
cmp_sigma_sqd[i] -= cmp_mu[i] * cmp_mu[i];
ref_sigma_sqd[i]  = MAX(0.0, ref_sigma_sqd[i]);
cmp_sigma_sqd[i]  = MAX(0.0, cmp_sigma_sqd[i]);
sigma_both[i]    -= ref_mu[i] * cmp_mu[i];
```

Both are pure **elementwise float** — no horizontal reduction, no
cross-type mixing. `_mm256_mul_ps` / `_mm512_mul_ps` on the same
inputs produce the same float result as scalar `a*b` under IEEE 754.
`_mm256_sub_ps` same for subtraction. `_mm256_max_ps` is bit-exact.
Nothing to fix.

### Where accumulate went wrong

Scalar `ssim_accumulate_default_scalar`:

```c
float  srsc = sqrtf(ref_sigma_sqd[i] * cmp_sigma_sqd[i]);
double lv = (2.0 * ref_mu[i] * cmp_mu[i] + C1) /         // <── DOUBLE numerator
            (ref_mu[i] * ref_mu[i] + cmp_mu[i] * cmp_mu[i] + C1);
double cv = (2.0 * srsc + C2) /                          // <── DOUBLE numerator
            (ref_sigma_sqd[i] + cmp_sigma_sqd[i] + C2);
float  csb = (sigma_both[i] < 0.0f && srsc <= 0.0f) ? 0.0f : sigma_both[i];
double sv  = (csb + C3) / (srsc + C3);                   // float → double on assign
ssim_sum  += lv * cv * sv;                               // DOUBLE × DOUBLE × DOUBLE
```

Two C type-promotion rules are load-bearing:

1. **`2.0` is a C `double` literal.** `2.0 * ref_mu[i]` promotes the
   float `ref_mu[i]` to double before the multiply. The full
   numerators of `lv` and `cv` are evaluated in double.
2. **`double sv = <float expr>`** promotes the float division result
   to double at assignment. `lv * cv * sv` then runs as three
   double multiplies.

Prior SIMD computed everything as vector float:

```c
// prior (drifted)
__m256 v2    = _mm256_set1_ps(2.0f);                     // <── FLOAT 2.0f
__m256 l_num = _mm256_mul_ps(v2, _mm256_mul_ps(rm, cm)) + vC1;  // float numerator
__m256 l     = _mm256_div_ps(l_num, l_den);              // float l
... // same for c and s
__m256 ssim_val = _mm256_mul_ps(_mm256_mul_ps(l, c), s); // float × float × float
// store, then accumulate to double
for (k=0..7) local_ssim += (double)t_ssim[k];
```

Two bit-exactness breaks: the `2.0f` numerator is single-precision,
not double, and the `l*c*s` product runs in single-precision, not
double. Both together are where the ~0.13 float-ULP comes from,
amplified over ~10 M accumulations across 5 pyramid scales.

### Fix pattern

Split the reduction: keep SIMD for the *float-valued intermediates*
that are already bit-exact against scalar, and move the
double-precision numerator + divide + triple product into a scalar
inner loop over lanes. Per-lane reduction order 0..7 (or 0..15)
matches scalar's `for i in 0..n` running sum byte-for-byte.

```c
// SIMD (float — bit-exact vs scalar):
__m256 srsc  = _mm256_sqrt_ps(rs * cs);        // sqrtf(rs*cs)
__m256 l_den = ((rm*rm + cm*cm) + vC1);        // float
__m256 c_den = ((rs + cs) + vC2);              // float
__m256 sv_f  = (clamped_sb + vC3) / (srsc + vC3);
// store rm, cm, srsc, l_den, c_den, sv_f to aligned temps.

// Scalar double per lane (bit-exact vs scalar):
for (int k = 0; k < LANES; k++) {
    double lv = (2.0 * t_rm[k] * t_cm[k] + C1) / t_l_den[k];
    double cv = (2.0 * t_srsc[k]  + C2)         / t_c_den[k];
    double sv = t_sv[k];                        // float → double
    local_ssim += lv * cv * sv;
    local_l    += lv;
    local_c    += cv;
    local_s    += sv;
}
```

LANES = 8 for AVX2 (`_Alignas(32)` float[8] temps), 16 for AVX-512
(`_Alignas(64)` float[16] temps). Scalar tail for `n % LANES`
unchanged.

### Why not "full SIMD double" instead

Alternative considered: widen everything to `__m256d` / `__m512d`
and do the 2.0 multiply + divide + triple product entirely in vector
double. Dropped because:

- Every input needs `_mm256_cvtps_pd` widening at use — for
  `2.0 * rm * cm`, we'd still need to widen `rm` and `cm`
  separately, then multiply in double. The `float → double`
  promotion is free in scalar C; in SIMD it's an explicit widening
  op.
- Effective lane count halves (`__m256d` = 4 lanes, `__m512d` = 8
  lanes) so throughput parity with the chosen per-lane-scalar
  design is a wash — both spend LANES scalar-equivalent cycles on
  the numerator/divide/product work per SIMD block.
- Double divide is ~20-30 cycles latency and serialises per lane
  anyway on modern cores; gaining nothing from SIMD.

### Why not FMA

Scalar `2.0 * ref_mu[i] * cmp_mu[i] + C1` has two rounding points:
one at each `*` and one at the final `+`. FMA collapses the last
`mul + add` into a single round — which is closer to the infinitely
precise result, but different from scalar. Bit-exactness is not the
same as "more accurate". Scalar wins by definition here.

### Verification

After the rewrite:

```text
$ diff <(grep -v '<fyi fps' /tmp/scalar.xml) \
       <(grep -v '<fyi fps' /tmp/avx2.xml)
$ diff <(grep -v '<fyi fps' /tmp/scalar.xml) \
       <(grep -v '<fyi fps' /tmp/avx512.xml)
$ # Both empty → bit-identical at --precision max.
```

Verified on:

- Netflix canonical pair `src01_hrc00_576x324` ↔ `src01_hrc01_576x324`
- Checkerboard 1-pixel shift (`checkerboard_1920_1080_10_3_0_0` ↔ `..._1_0`)

Both `float_ssim` and `float_ms_ssim` now match scalar byte-for-byte
across scalar / AVX2 / AVX-512 dispatch.

## Open questions / follow-ups

- **NEON parity?** The fork ships
  [`ssim_neon.c`](../../core/src/feature/arm64/ssim_neon.c)
  with the same three dispatch entry points. The same pattern
  applies — this research digest covers x86 only. Defer to a
  follow-up: confirm NEON is bit-exact at `--precision max` on an
  Apple Silicon or aarch64 Linux host and apply the same fix if
  not.
- **Perf impact.** 8 / 16 scalar doubles per SIMD block in the
  reduction inner loop. Hot ops (sqrt, divides, clamps) stay
  vectorised. Expected perf delta is small (single-digit percent)
  but should be measured before the next release.
- **Assertion precision.** The fork's bit-exactness guarantee at
  `--precision max` is stronger than Netflix's golden
  `places=N` asserts need. The golden gate was never in danger
  (0.13 ULP < `places=3` / `places=4` thresholds), but the
  `--precision max` CLI tool is the surface where users actually
  see the drift.

## References

- Source: user popup (2026-04-21) — "Fix to bit-exact now (extend
  this PR)".
- ADR: [ADR-0139](../adr/0139-ssim-simd-bitexact-double.md).
- Companion: [ADR-0138](../adr/0138-iqa-convolve-avx2-bitexact-double.md)
  and [Research-0011](0011-iqa-convolve-avx2.md).
- Prior attempt: PR #18 (`f082cfd3`) "SIMD bit-identical reductions
  CI fixes".

# ADR-0139: SSIM SIMD accumulate bit-exact to scalar via per-lane scalar double

- **Status**: Accepted
- **Date**: 2026-04-21
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: simd, performance, bit-exact

## Context

The fork ships AVX2 and AVX-512 SIMD paths for SSIM and MS-SSIM at
[`core/src/feature/x86/ssim_avx2.c`](../../core/src/feature/x86/ssim_avx2.c)
and
[`core/src/feature/x86/ssim_avx512.c`](../../core/src/feature/x86/ssim_avx512.c),
introduced in commit `81fcd42e`. Upstream Netflix/vmaf has AVX2 /
AVX-512 for VIF / ADM / motion / CAMBI but no SSIM SIMD at all — the
SSIM SIMD surface is entirely fork-local. PR #18 (`f082cfd3`)
tightened float accumulations across the fork and claimed SSIM SIMD
was bit-identical to scalar.

During the [ADR-0138](0138-iqa-convolve-avx2-bitexact-double.md)
convolve work, a CLI XML diff at `--precision max` on Netflix
`src01_hrc00/01_576x324` and `checkerboard_1920_1080_10_3_0_0/1_0`
showed a residual divergence between scalar and SIMD at the 8th
decimal on `float_ms_ssim`:

| Fixture | Scalar | AVX2 / AVX-512 | Δ |
| --- | --- | --- | --- |
| checkerboard frame 0 | `0.89452544363435227` | `0.89452545888605872` | ≈ 1.5e-8 |
| checkerboard frame 1 | `0.93540067693522488` | `0.93540069286261140` | ≈ 1.6e-8 |

`float_ssim` was already bit-identical across scalar / AVX2 / AVX-512
on these fixtures. Isolating the dispatch setters confirmed the
divergence sits in `ssim_accumulate_*`, not in `_iqa_convolve_*`
or `ssim_precompute_*` / `ssim_variance_*`:

- `ssim_precompute_*` is three pure elementwise float multiplies
  (`ref*ref`, `cmp*cmp`, `ref*cmp`). Vector `_mm256_mul_ps` on the
  same inputs is bit-identical to scalar `a*b`. No reduction.
- `ssim_variance_*` is an elementwise subtract + clamp-to-zero.
  Again pure float, no reduction. Bit-identical.
- `ssim_accumulate_*` is where the divergence lives.

Reading scalar `ssim_accumulate_default_scalar` in
[`ssim_tools.c:174-205`](../../core/src/feature/iqa/ssim_tools.c#L174-L205):

```c
float  srsc = sqrtf(ref_sigma_sqd[i] * cmp_sigma_sqd[i]);
double lv = (2.0 * ref_mu[i] * cmp_mu[i] + C1) /
            (ref_mu[i] * ref_mu[i] + cmp_mu[i] * cmp_mu[i] + C1);
double cv = (2.0 * srsc + C2) / (ref_sigma_sqd[i] + cmp_sigma_sqd[i] + C2);
float  csb = (sigma_both[i] < 0.0f && srsc <= 0.0f) ? 0.0f : sigma_both[i];
double sv  = (csb + C3) / (srsc + C3);      /* float div → promoted */
ssim_sum  += lv * cv * sv;                  /* double * double * double */
```

Two type-promotion rules govern bit-exactness here:

1. **`2.0 * ref_mu[i]` is double.** The literal `2.0` is a `double`
   under C89/C99; `double * float` promotes the float to double
   before the multiply. Scalar thus computes the `l` and `c`
   numerators in double precision.
2. **`double sv = float_expr`** promotes the float quotient to
   double at assignment, and `lv * cv * sv` runs as three double
   multiplies.

The prior SIMD code computed `l`, `c`, and `s` all as `__m256` /
`__m512` float (`_mm256_mul_ps(v2, rm*cm)`, then `_mm256_div_ps`),
then `_mm256_mul_ps(l*c) * s` before storing to an aligned float
temp and accumulating into `double`. That collapses the double-path
numerators and the double-path triple product into float-path
operations — visible as ~0.13 float-ULP drift after summation over
a 1920 × 1080 grid at 5 pyramid scales.

Two fork rules shape the fix:

- [CLAUDE.md §8](../../CLAUDE.md#8-netflix-golden-data-gate-do-not-modify)
  freezes the Netflix CPU golden `assertAlmostEqual(places=N)`
  asserts. `places=3` for SSIM and `places=4` for MS-SSIM left
  ~0.13 ULP below the assertion threshold, so the prior SIMD
  still passed the golden gate — but the drift was visible at
  `--precision max` and violated the fork's stated
  "AVX2/AVX-512 float paths match scalar bit-for-bit" claim from
  PR #18.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) requires a
  documented rationale for any numerical fix that alters SIMD
  output.

## Decision

Rewrite `ssim_accumulate_avx2` and `ssim_accumulate_avx512` to
compute the **float-valued intermediates** (`srsc`, `l_den`, `c_den`,
`clamped_sb`, `sv_float`) in SIMD vector float — these are
trivially bit-exact against scalar — and then do the
**double-precision numerator, double division, and double triple
product** per-lane in scalar double inside an inner
`for (k = 0; k < LANES; k++)` loop that matches scalar C's type
promotions exactly:

```c
/* SIMD (float): srsc, l_den, c_den, sv_float, clamp mask. */
const __m256 srsc  = _mm256_sqrt_ps(_mm256_mul_ps(rs, cs));
const __m256 l_den = ((rm*rm + cm*cm) + vC1);                 /* float */
const __m256 c_den = ((rs + cs) + vC2);                       /* float */
const __m256 sv_f  = _mm256_div_ps(clamped_sb + vC3, srsc + vC3);

/* store to aligned temps, then scalar double per lane */
for (int k = 0; k < 8; k++) {
    const double lv = (2.0 * t_rm[k] * t_cm[k] + C1) / t_l_den[k];  /* double */
    const double cv = (2.0 * t_srsc[k] + C2) / t_c_den[k];           /* double */
    const double sv = t_sv[k];                                        /* f32→f64 */
    local_ssim += lv * cv * sv;
    local_l    += lv;
    local_c    += cv;
    local_s    += sv;
}
```

Reduction order (lane 0 → lane LANES-1 within each block) matches
scalar's `for i in 0..n` exactly, so the running-sum
associativity is byte-identical. AVX-512 uses 16 lanes per block
with `_Alignas(64)` 16-element temp buffers; AVX2 uses 8 lanes
with `_Alignas(32)` 8-element temps. Both keep the same scalar
tail for `n % LANES`.

`ssim_precompute_*` and `ssim_variance_*` keep their current pure
SIMD float paths — they are already bit-exact by virtue of being
non-reducing elementwise float ops.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Per-lane scalar double reduction (chosen) | Matches scalar C type promotions exactly; preserves SIMD speedup on the compute-heavy float ops (`srsc`, divisions, clamp); simple to reason about | Loses some parallelism on the double ops; 8 / 16 scalar divides per SIMD block | **Decision** — only pattern that preserves scalar rounding without redesigning the caller |
| Full SIMD double path (`__m256d` / `__m512d`) with all ops in double | Theoretically preserves bit-exactness in-lane | Requires widening every input to double, doubling register pressure and cache pressure; the `2.0 * rm * cm + C1` promotion rule still needs `float→double` widening of `rm, cm` first before each use; effective lane count halves (4 / 8) so throughput is similar to the chosen design | Rejected — more complex, not obviously faster |
| FMA in double after widening | Fewer rounding points per tap | FMA collapses `mul + add` into one rounding, diverging from scalar's two-step `sum += a*b` | Rejected — bit-exactness lost |
| Revert: disable the SSIM SIMD dispatch entirely | Guaranteed bit-exact (falls to scalar); ~20-40% MS-SSIM CPU regression | Gives up the fork's SSIM SIMD feature | Rejected — user popup 2026-04-21 chose "fix bit-exact now" over "revert" and "keep as-is" |
| Keep as-is, document the ULP budget | Zero code churn | Contradicts PR #18's "bit-identical" claim; drift visible at `--precision max`; complicates future SIMD ports that want to compare against scalar | Rejected — once known, not fixing it is a regression of the stated invariant |

## Consequences

- **Positive**: `float_ssim` and `float_ms_ssim` are now
  byte-for-byte identical between scalar, AVX2, and AVX-512 at
  `--precision max` on both the Netflix normal pair and the
  checkerboard pair. The fork's "SIMD float paths match scalar
  bit-for-bit" claim (PR #18) holds on the SSIM surface too.
  Netflix golden `assertAlmostEqual(places=N)` asserts unchanged
  (they already passed before; they still pass).
- **Negative**: The SIMD inner loop does 8 / 16 scalar doubles
  per block instead of vector float for the final reduction
  stage. Expected perf impact is small because the compute-heavy
  ops (srsc, divisions) stay vectorised, but the bench should
  confirm. Added ~30 LoC per SIMD file (temp buffers + scalar
  reduction loop) against ~20 LoC removed.
- **Neutral / follow-ups**:
  - `core/src/feature/AGENTS.md` rebase invariant: any future
    upstream-port that changes `ssim_accumulate_default_scalar`
    **must** preserve the `2.0 *` double-precision literal and
    the `float → double` promotion of `sv` — both are load-bearing
    for the SIMD bit-exactness match. If upstream changes the
    scalar, both SIMD variants need a matching change.
  - `CHANGELOG.md` "lusoris fork" entry under "Fixed".
  - `docs/rebase-notes.md` entry documenting the per-lane scalar
    reduction pattern so rebase conflicts on `ssim_accumulate_*`
    are resolved in favour of the fork's bit-exact pattern.
  - Reproducer (for PR description):

    ```text
    vmaf --cpumask 255 ... --feature float_ms_ssim --precision max -o scalar.xml
    vmaf --cpumask 16  ... --feature float_ms_ssim --precision max -o avx2.xml
    vmaf              ... --feature float_ms_ssim --precision max -o avx512.xml
    diff <(grep -v '<fyi fps' scalar.xml) <(grep -v '<fyi fps' avx2.xml)    # empty
    diff <(grep -v '<fyi fps' scalar.xml) <(grep -v '<fyi fps' avx512.xml)  # empty
    ```

## References

- Source: user popup (2026-04-21) — "Fix to bit-exact now (extend
  this PR)" chosen over revert / dedicated-follow-up / keep-as-is
  after the convolve PR investigation surfaced the residual
  ~0.13 float-ULP drift on MS-SSIM.
- Related ADRs:
  [ADR-0138](0138-iqa-convolve-avx2-bitexact-double.md) —
  companion bit-exact convolve fast path.
  [ADR-0108](0108-deep-dive-deliverables-rule.md) — six deep-dive
  deliverables apply.
- Scalar reference:
  [`ssim_accumulate_default_scalar`](../../core/src/feature/iqa/ssim_tools.c#L174-L205)
  in `ssim_tools.c`.
- Prior attempt at bit-exactness:
  PR #18 (`f082cfd3`) "SIMD bit-identical reductions + CI fixes".

### Status update 2026-05-08: Accepted

Audited as part of the 2026-05-08 ADR `Proposed` sweep
([Research-0086](../research/0086-adr-proposed-status-sweep-2026-05-08.md)).

Acceptance criteria verified in tree at HEAD `0a8b539e`:

- `core/src/feature/x86/ssim_avx2.{c,h}` and
  `ssim_avx512.{c,h}` carry the per-lane scalar-double reduction
  pattern.
- ADR-0140 codifies the reduction macro
  (`SIMD_PER_LANE_SCALAR_DOUBLE_REDUCE_AVX2/AVX512`) and cites this
  ADR as the load-bearing rationale for the inline form.
- Verification command:
  `ls core/src/feature/x86/ssim_avx2.{c,h}
  core/src/feature/x86/ssim_avx512.{c,h}`.

<!-- markdownlint-disable MD013 -->

# Research-2031 — `cpp/integer-multiplication-cast-to-long` on convolve, moment, and PSNR

**Date**: 2026-09-06
**Follow-up**: 2026-09-24 (alert 1005 executable domain proof and source suppression)
**Scope**: CodeQL alert 1005 (`core/src/feature/iqa/convolve.c:155`) remains
open on `master`; alert 1009 (`core/src/feature/psnr.c:43`) is fixed. The
follow-up also covers dismissed false-positive alert 707
(`core/src/feature/moment.c:61`).
**Outcome**: 1009 is fixed. Alert 707's source pattern is removed with an
explicit widening after its single-rounded float product. Alert 1005 is a
proved production-domain false positive whose direct float multiply is
load-bearing under
[ADR-0138](../adr/0138-iqa-convolve-avx2-bitexact-double.md); executable domain
and scalar/SIMD regressions now back a single-query source suppression, with no
arithmetic change.

The follow-up began from exact `origin/master`
`4e6916d16ac57647105d14a47a6680117d6b5738`. GitHub's live alert API still
reported alert 1005 open there under CodeQL 2.27.0, at the vertical-pass
`float * float` expression. That hosted result is the red baseline; hosted
closure necessarily waits for this source change to reach `master` and for the
next scan.

## What the rule actually says here

Despite the rule id, neither alert is about integer arithmetic. Both carry the
message *"Multiplication result may overflow 'float' before it is converted to
'double'"*. The pattern is `double_accumulator += float_a * float_b;` — C
evaluates the product in `float` and widens the result, so a product outside
float range is lost before the wider type ever sees it.

## Alert 1009 — `psnr.c:43`, fixed

```c
float diff = ref_ - dis_;
noise_ += diff * diff;          /* -> (double)diff * diff */
```

Fixed by widening one operand so the multiply happens in `double`.

Two facts make this risk-free, and both were checked rather than assumed:

- `compute_psnr()` has **no call sites**. `grep -rn compute_psnr` across every
  `.c` / `.h` / `.cpp` in the tree returns only its declaration in `psnr.h:32`
  and its definition in `psnr.c:30`, and `nm` on `liblibvmaf_feature.a` shows
  `T compute_psnr` with no matching `U` anywhere. The live PSNR paths are
  `integer_psnr.c` and `float_psnr.c`. *(Follow-up, not actioned here: this is a
  dead twin of `float_psnr.c` and a candidate for the next dead-twin collapse
  pass — see commit `d3f97db4f`.)*
- It has no SIMD twin. The AVX2 / AVX-512 / NEON PSNR kernels mirror
  `float_psnr_noise_line_c()` in `float_psnr.c`, not this function.

Measured: all three Netflix golden pairs are bit-identical before and after at
`--precision=max` (0 differing keys across every metric), and
`meson test --suite=fast` is unchanged.

## Alert 1005 — `convolve.c:155`, bounded and suppressed in source

Pre-widening only the exact vertical-pass expression from alert 1005 **breaks
the build's own bit-exactness test**:

```text
test_picture_copy_sample_bound: pass
test_ms_ssim_decimate_gain_bound: pass
test_pu21_sample_bound: pass
test_convolve_kernel_bound: pass
test_convolve_product_bound: pass
test_domain_ceiling_convolve: pass
test_gauss_11x11: fail
avx2 convolve output not bit-identical to scalar
```

Control at the same commit is 19/19 pass. The mutation changes only
`img_cache[...] * k->kernel_v[k_offset]` to
`(double)img_cache[...] * k->kernel_v[k_offset]`; the bound checks still pass,
while the ordinary Gaussian fixture supplies the red numerical witness.

That is not a flaky test — it is the invariant ADR-0138 exists to protect. The
AVX2 twin computes the product in `float` and *then* widens, on purpose:

```c
const __m256  prod_f = _mm256_mul_ps(...);
const __m256d prod   = _mm256_cvtps_pd(prod_f);   /* widen AFTER the multiply */
acc = _mm256_add_pd(prod, acc);
```

`convolve_avx2.h` states the contract outright: *"Bit-identical to the scalar
reference by construction: `__m256d` (4-lane double) accumulator ... to mirror
the scalar's unfused `sum += a*b`."* Widening the scalar multiply to `double`
therefore desynchronises scalar from AVX2, AVX-512 and NEON at once, and would
allow scalar and SIMD SSIM / MS-SSIM scores to diverge.

The original `|a| <= 255` rationale was incomplete. `picture_copy()` can emit
values just below 256 for 10/12/16-bit pictures, MS-SSIM's signed 9/7
decimator can overshoot its input range, and PU21 calls `iqa_convolve()` with
encoded values near 600. The complete production-call-domain proof is:

1. The only production callers are SSIM/MS-SSIM through
   `iqa/ssim_tools.c::iqa_convolve_dispatch()` and PU21 through
   `pu21_ssim.c`. The direct calls in tests do not create a runtime input
   surface.
2. For every supported 8/10/12/16-bit input, `picture_copy(..., offset=0)`
   emits a sample in `[0, 2^8)`. Ordinary SSIM's optional box decimation is a
   positive average and cannot enlarge that bound.
3. MS-SSIM applies the signed separable 9-tap 9/7 filter four times. The
   production scalar implementation's interior impulse response has L1 norm
   below 2; border reflection only aliases terms and cannot exceed the same
   triangle-inequality bound. Thus every level-4 sample has magnitude below
   `2^8 * 2^4 = 2^12`, and its square/cross-product inputs to convolve are
   below `2^24`.
4. For every PU21 coefficient row, `p1 > p0*p2`, both exponents are positive,
   and the scale is positive. Therefore the rational inner term and complete
   encoder are monotone on `[0.005, 10000]`; evaluating the upper endpoint
   gives a value below `2^10`. Its square/cross-product inputs are below
   `2^20`, smaller than the MS-SSIM bound.
5. Gaussian convolve has at most 11 taps per pass (box has 8), and every tap
   magnitude is below 1. Even the deliberately loose bound of eleven `2^24`
   horizontal products fits below `2^28` after the float cache store. The
   flagged vertical product multiplies that cache by another sub-unity tap, so
   it also stays at or below `2^28`, about 100 binary exponents below IEEE-754
   `FLT_MAX`.

`test_iqa_convolve` now derives the signed decimator gain through the production
implementation, checks the real `picture_copy()` maxima at all supported bit
depths, proves PU21 monotonicity and its endpoint bound, executes the flagged
convolve at the `2^24` stats-domain ceiling, and requires byte identity with
every available SIMD twin. These tests run their scalar/domain portion even on
hosts without a supported SIMD ISA.

Widening the scalar **and** all three SIMD twins would instead be a numerical
algorithm change: it would require re-deriving ADR-0138, re-baselining every
SSIM/MS-SSIM snapshot, and accepting the SIMD cost of double multiplication.
There is no security basis for that change because the proved production
domain cannot approach overflow.

The selected resolution is the official narrow CodeQL source-suppression form,
`// codeql[cpp/integer-multiplication-cast-to-long]`, on the standalone line
immediately before the exact flagged expression. The shared CodeQL suppression
parser scopes that directive to the following line only; see
[`AlertSuppression.qll`](https://github.com/github/codeql/blob/codeql-cli/v2.27.0/shared/util/codeql/util/suppression/AlertSuppression.qll).
This closes the false positive durably without a UI-only dismissal or a
semantic rewrite.

Alert 707 in `moment.c` has a different, tolerance-bounded contract under
[ADR-0179](../adr/0179-float-moment-simd.md) and
[ADR-0987](../adr/0987-avx512-float-moment.md). Its square remains a
single-rounded float operation, then widens explicitly for the double
accumulator:

```c
const float term = pic_ * pic_;
cum += (double)term;
```

That source rewrite removes the historical query pattern without claiming the
byte-exact scalar/SIMD contract that applies to convolve.

## Alternatives considered

| Alternative | Result | Decision |
| --- | --- | --- |
| Pre-widen only the scalar operand | Breaks byte identity with AVX2/AVX-512/NEON | Rejected by ADR-0138 and the mutation test |
| Convert scalar and every SIMD twin to double multiply | Changes metric numerics and throughput for an unreachable overflow | Rejected |
| Dismiss alert 1005 only in the GitHub UI | Leaves the rationale and closure outside source control | Rejected |
| Add a query-wide workflow exclusion | Hides real findings from the same query elsewhere | Rejected |
| Prove the domain and suppress this expression only | Preserves required arithmetic and remains reviewable/rebase-visible | Selected |

No new ADR is needed: ADR-0138 already decides the arithmetic contract. This
digest supplies the previously missing domain proof and suppression decision.

## Reproducing the convolve result

```bash
# Passing control, including the executable domain proof:
meson setup build -Denable_cuda=false -Denable_sycl=false
ninja -C build test/test_iqa_convolve
./build/test/test_iqa_convolve

# Red mutation: temporarily change the exact flagged vertical product from
#   sum += img_cache[...] * k->kernel_v[k_offset];
# to
#   sum += (double)img_cache[...] * k->kernel_v[k_offset];
# then rebuild and rerun. Restore that one-line patch afterwards.
ninja -C build test/test_iqa_convolve
./build/test/test_iqa_convolve        # must fail scalar/SIMD byte identity
```

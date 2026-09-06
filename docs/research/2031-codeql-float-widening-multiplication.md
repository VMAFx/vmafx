<!-- markdownlint-disable MD013 -->

# 2031 — `cpp/integer-multiplication-cast-to-long` on the float convolve and PSNR paths

**Date**: 2026-09-06
**Scope**: CodeQL alerts 1005 (`core/src/feature/iqa/convolve.c:155`) and
1009 (`core/src/feature/psnr.c:43`), both `high`, both open on `master`.
**Outcome**: 1009 fixed; 1005 analysed and **deliberately not fixed** — the
float multiply it flags is load-bearing under [ADR-0138](../adr/0138-iqa-convolve-avx2-bitexact-double.md).

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

## Alert 1005 — `convolve.c:155`, not fixed

The same one-character change to `iqa_convolve`'s four accumulation sites
(lines 134, 155, 200, 295) **breaks the build's own bit-exactness test**:

```text
test_gauss_11x11: pass
test_gauss_12x12: fail
avx2 convolve output not bit-identical to scalar
```

Control at the same commit is 13/13 pass.

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
change every SSIM / MS-SSIM score the fork produces.

The overflow the rule warns about is also unreachable on this data path. The
inputs are image samples convolved with a normalised Gaussian or box kernel:
`|a| <= 255` (or `<= 1.0` normalised) and `|b| <= 1`, so `|a * b| <= 255`, some
38 orders of magnitude below `FLT_MAX`. There is no input to
`iqa_convolve()` — border reflection included — that can drive a single product
out of float range.

Fixing it properly would mean widening the scalar **and** all three SIMD twins
to a double multiply, re-deriving ADR-0138's bit-exactness construction,
re-baselining every SSIM / MS-SSIM snapshot, and paying the SIMD throughput
cost of halving the lanes per multiply. That is a numerics change to the
metric, not a security fix, and it is not justified by an overflow that the
input domain forbids.

**Recorded as reported-not-fixed**, matching how the `py/cyclic-import` pair was
handled in [2028](2028-code-scanning-audit-2026-09-03.md). Per the standing
project rule, agents analyse and fix; the maintainer decides dismissals — this
alert is left open in the UI deliberately, with this digest as its rationale.

## Reproducing the convolve result

```bash
# in a worktree at origin/master
sed -i 's/sum += img\[img_offset + u\] \* k->kernel_h\[k_offset\];/sum += (double)img[img_offset + u] * k->kernel_h[k_offset];/' \
    core/src/feature/iqa/convolve.c   # and the three sibling sites
meson setup build -Denable_cuda=false -Denable_sycl=false && ninja -C build
./build/test/test_iqa_convolve        # test_gauss_12x12 fails
```

<!-- markdownlint-disable MD013 MD060 -->
# ADR-1413: Every integer ADM implementation bounds the enhancement gain with the scalar's truncated double product

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: adm, correctness, numerics, simd, sycl, fork-local

## Context

Integer ADM's decouple stage limits how much of a contrast enhancement counts
as restored signal. Where the reference and distorted coefficient vectors point
the same way, the scalar kernels (`adm_decouple_band()` and
`adm_decouple_band_s123()` in `core/src/feature/integer_adm_kernels.h`) store

```c
rst = MIN(rst * gain, t);   /* or MAX(rst * gain, t) for a negative rst */
```

with `rst` and `t` integers and `gain` the double `adm_enhn_gain_limit`. The
product is a double, rounded to nearest, and the store into the integer
truncates it toward zero. The shipped models use a limit of 1 or 100, whose
products are integral, so any conversion gives the same sample. A non-integer
limit (the option admits [1, 100]) does not, and three implementations had each
picked a different conversion:

- The scale-0 AVX2 and AVX-512 kernels used `_mm256_cvtpd_epi32` /
  `_mm512_cvtpd_epi32` and the AVX-512 scale 1-3 kernel `_mm512_cvtpd_epi64`,
  which round to nearest. With a limit of 1.2 they were one off in every
  limited sample whose product has a fraction of one half or more
  (`T-ADM-DECOUPLE-X86-FRACTIONAL-GAIN-ROUNDING-2026-10-01`).
- The SYCL twin has no double on the device
  ([ADR-0220](0220-sycl-fp64-fallback.md)) and formed the product in Q31 fixed
  point, which floors and rounds the limit itself to 2^-31
  (`T-SYCL-ADM-FRACTIONAL-GAIN-LIMIT-2026-09-29`, deferred until now).
- The CUDA and HIP twins convert the double product with a C cast, which
  truncates, and already agree with the scalar.

Measured on `0fda8066a` (the head of pull request #1700 before it merged as
master `408dcaad5`; the x86, SYCL and CUDA ADM sources are the same in both)
at `--precision max` against the scalar path (`--cpumask 4294967295`), worst
frame of `integer_adm_scale0`:

| Input | AVX2, 1.2 | AVX-512, 1.2 | AVX2 and AVX-512, 1.5 | SYCL (Arc A380), 1.2 | SYCL, 1.5 |
|---|---|---|---|---|---|
| Netflix pair `src01` 576x324 | 1.23e-6 | 1.15e-6 | 3.07e-7 | 1.23e-6 | 3.07e-7 |
| Checkerboard 1920x1080, 1 px shift | identical | identical | 3.44e-5 | 1.65e-5 | 1.73e-5 |
| `akiyo` 352x288 | 6.23e-6 | 6.23e-6 | 6.33e-7 | 7.28e-6 | 6.33e-7 |
| Blurred blocks 576x324 | 3.56e-5 | 3.44e-5 | identical | 3.60e-5 | identical |
| Big Buck Bunny 3840x2160, 200 frames | 1.99e-6 | 1.99e-6 | 1.16e-6 | 2.31e-6 | 9.54e-7 |

With limits of 1 and 100 every implementation was identical on all 20 pairs.
On a picture whose contrast the distorted copy doubles the SYCL twin was
2.1e-4 off at 96x64, past the places=4 gate of
[ADR-0214](0214-gpu-parity-ci-gate.md).

The test written for this also showed a second difference in the scale-0
vector kernels. Their angle test forms the dot product and the squared
magnitudes with `_mm256_madd_epi16` / `_mm512_madd_epi16`, whose int32 sum
wraps in one case: both products are 2^30, the sum is 2^31 and the lane reads
`INT32_MIN`. With `h = v = -32768` in one picture the squared magnitude came
out negative and the angle flag was set where the scalar clears it. A decoded
picture does not get there (its scale-0 coefficients stay within about 22900),
but the kernels accept the operands.

## Decision

The scalar kernels define the limit, and every implementation that can be run
returns their sample for every limit the option admits.

- **Why truncating first is enough.** `t` is an integer, so
  `trunc(min(x, t)) == min(trunc(x), t)` and likewise for `max`. An
  implementation needs `trunc(fl(rst * gain))` and an integer minimum or
  maximum.
- **AVX2 and AVX-512** convert the double product with the truncating forms
  (`_mm256_cvttpd_epi32`, `_mm512_cvttpd_epi32`, `_mm512_cvttpd_epi64`) in
  `decouple_gain_avx2()`, `decouple_gain_avx512()` and
  `decouple_s123_limit_half_avx512()`. The AVX2 scale 1-3 kernel already
  truncated.
- **The scale-0 angle test** reads the squared magnitudes as unsigned and the
  dot product as 2^31 where it is `INT32_MIN`
  (`decouple_angle_mask_avx2()`, `decouple_angle_mask_avx512()`).
- **SYCL** forms `trunc(fl(rst * gain))` from 64-bit integer arithmetic with
  `adm_gain_limit_product()` in the new `core/src/feature/adm_gain_limit.h`.
  The limit travels as its 53-bit significand in two halves and the position
  of its binary point. The exact product `P = |rst| * M` is at most 84 bits;
  its integer part `N` and fraction `F` come from two 64-bit products, and the
  double product truncates to `N + 1` instead of `N` exactly when `F` lies
  within half a unit in the last place below the next integer, which reduces
  to a comparison of two bit lengths and needs no bit scan. The header carries
  the derivation.
- **CUDA and HIP** are unchanged: every double-to-integer conversion in the
  CUDA ADM kernels compiles to `cvt.rzi.s32.f64` (round toward zero).
- **NEON and SVE2** have no decouple kernel; aarch64 runs the scalar code.
- **Metal** multiplies in binary32 (`(int)((float)rst * egl)`), which is
  neither the double product nor exact for a product above 2^24. It is not
  changed here: no Apple device was available to run it
  (`T-METAL-ADM-GAIN-LIMIT-FLOAT32-2026-10-01`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Make the scalar round to nearest, as the vector kernels did | Two of three x86 paths already did it; no golden moves, since the goldens use integral limits | The scalar is upstream's reference, and CUDA and HIP truncate like it; the scalar's result under a non-integer limit would change | The scalar is the reference by project rule ([ADR-0138](0138-iqa-convolve-avx2-bitexact-double.md), [ADR-0139](0139-ssim-simd-bitexact-double.md)) |
| SYCL: keep Q31 and truncate the magnitude instead of flooring | Smallest kernel change | Still not the double product: the limit is rounded to 2^-31 and the product is not rounded to 53 bits, so `5 * 1.2` gives 5 where the CPU stores 6 | Not exact; one sample in five differs at 1.2 |
| SYCL: an fp64 variant of the kernels, chosen by device aspect | Exact by construction where fp64 exists | One fp64 instruction blocks the whole SPIR-V module on Arc A-series (ADR-0220); two kernel variants to keep equal; the A-series twin would stay inexact | Does not fix the device the twin is run on |
| SYCL: emulate the multiply literally, a 128-bit product rounded to 53 bits and then truncated | A direct transcription of IEEE 754 | Needs a 128-bit shift and a bit scan per sample in device code | The comparison form returns the same value from two 64-bit products |
| Leave the SYCL row deferred | No kernel change | The twin stays up to 3.6e-5 from the CPU on ordinary content at a limit of 1.2 and beyond places=4 on enhanced content | The fix was already described in the row and fits in one header |
| Bound the scale-0 test bands to the range a decoded picture reaches instead of fixing the angle test | No kernel change | The vector kernels would keep an operand on which they differ from the scalar | The fork's rule is that SIMD equals the scalar on every operand the kernel accepts |

## Consequences

- **Positive**:
  - Scalar, AVX2, AVX-512 and the SYCL twin give identical JSON at
    `--precision max` for limits of 1, 1.2, 1.5 and 100 on the 13 fixture
    pairs under `python/test/resource/yuv/`, on seven synthetic pairs and on
    Big Buck Bunny 3840x2160 (200 frames).
  - `adm_gain_limit.h` is one definition a further fp64-less backend can
    include.
  - `T-SYCL-ADM-FRACTIONAL-GAIN-LIMIT-2026-09-29` closes with
    `T-ADM-DECOUPLE-X86-FRACTIONAL-GAIN-ROUNDING-2026-10-01`.
- **Negative**:
  - With a non-integer limit the x86 and SYCL results move by the amounts in
    the table above. Nothing moves at the defaults or under any shipped
    model: the Netflix golden gate is 271 passed, 12 skipped before and
    after. Its three assertions at a limit of 1.2 (integer extractor, the
    352x288 `akiyo` pair, places=5) keep their values to that precision and
    move toward the golden numbers, e.g. `adm2` 1.116610457808281 to
    1.1166096821570612 against a golden 1.116609.
  - With a non-integer limit the fork's AVX2 and AVX-512 output equals
    upstream master's scalar output and no longer its vector output, which
    rounds.
  - The SYCL decouple does a few more 64-bit operations per limited sample:
    `adm_sycl` on Big Buck Bunny 3840x2160 takes 11.04 ms per frame where it
    took 10.88 (Arc A380, median of 5). The x86 decouple stages are within
    2% of their previous times. The research digest has the tables.
- **Neutral / follow-ups**:
  - The Metal twin still multiplies in binary32
    (`T-METAL-ADM-GAIN-LIMIT-FLOAT32-2026-10-01`).
  - The CUDA twin differs from the scalar by up to 2.9e-7 at every limit,
    100 included; that comes from its host finalisation, not from the limit.
    The HIP twin is bit-identical on 19 of 21 pairs at every limit.

## References

- Task brief (rc3 worker assignment, 2026-10-01): "The scalar code is the
  reference: make AVX2 and AVX-512 (and NEON/SVE2 if they have the path)
  bit-identical to it for integer and non-integer gains, with a SIMD-vs-scalar
  test over gains {1.0, 1.2, 1.5, 100.0} that fails on master. Check the CUDA,
  HIP and SYCL twins for the same rounding and fix those you can run".
- [Research-1413](../research/1413-adm-gain-limit-truncated-double-product.md):
  per-input deltas, the derivation of the integer product, planted defects,
  device results and timings.
- [ADR-0220](0220-sycl-fp64-fallback.md) (no fp64 in SYCL kernels; its Q31
  gain path is what this replaces),
  [ADR-1362](1362-sycl-integer-adm-aim-device-pass.md) (the SYCL twin is
  bit-exact with the CPU),
  [ADR-1194](1194-adm-angle-flag-single-source.md) (the shared angle predicate),
  [ADR-1402](1402-adm-cm-centre-tap-int32.md) (pull request #1700, where the
  x86 difference was found), [ADR-0024](0024-netflix-golden-preserved.md)
  (golden gate).
- `docs/state.md`: `T-ADM-DECOUPLE-X86-FRACTIONAL-GAIN-ROUNDING-2026-10-01`,
  `T-SYCL-ADM-FRACTIONAL-GAIN-LIMIT-2026-09-29`,
  `T-METAL-ADM-GAIN-LIMIT-FLOAT32-2026-10-01`.

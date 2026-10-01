<!-- markdownlint-disable MD013 MD060 -->
# Research-1413: The integer ADM enhancement gain limit under a non-integer limit

- **Status**: Active
- **Workstream**: [ADR-1413](../adr/1413-adm-gain-limit-truncated-double-product.md)
- **Last updated**: 2026-10-01

## Question

With a non-integer `adm_enhn_gain_limit`, which integer ADM implementations leave the scalar kernels, by how much on which inputs, and what must each compute to return the scalar's sample? In particular, can a backend without binary64 on the device return the scalar's truncated double product exactly?

## Sources

- `core/src/feature/integer_adm_kernels.h`: `adm_decouple_band()`, `adm_decouple_band_s123()` (the reference).
- `core/src/feature/x86/adm_avx2.c`, `core/src/feature/x86/adm_avx512.c`: `decouple_gain_*()`, `decouple_s123_limit_half_avx512()`, `decouple_angle_mask_*()`.
- `core/src/feature/sycl/integer_adm_sycl.cpp`: `adm_dev_gain_limit()`; `core/src/feature/adm_gain_limit.h` (new).
- `core/src/feature/cuda/integer_adm/adm_decouple_inline.cuh`, `core/src/feature/hip/integer_adm/adm_decouple_inline.hip`, `core/src/feature/metal/integer_adm.metal`.
- Host: Ryzen 9 9950X3D, RTX 4090 (CUDA), gfx1036 (HIP), Arc A380 under the Linux xe driver (SYCL), gcc 16.2.1, icpx 2026.0. Before: `0fda8066a`, the head of pull request #1700 before it merged as master `408dcaad5`; the x86, SYCL and CUDA ADM sources are the same in both. No Apple device.

## Findings

### What the scalar stores

`rst = MIN(rst * gain, t)` assigns a double to an integer. C converts by truncating toward zero. Because `t` is an integer, `trunc(min(x, t)) == min(trunc(x), t)` and `trunc(max(x, t)) == max(trunc(x), t)`: for `x < t` the truncated `x` cannot pass `t`, and for `x >= t` it cannot fall below it. Every implementation therefore needs `trunc(fl(rst * gain))`, where `fl` is the round-to-nearest double product, followed by an integer minimum or maximum.

The rounding inside `fl` matters. The double nearest 1.2 is `1.1999999999999999556`, and five times it is `5.9999999999999997780`, but the double product of 5 and that value is exactly 6. One sample in five has a product that rounds up to an integer in this way at a limit of 1.2 (20000 of the values 1 to 100000).

### Before: scalar against each implementation

Worst frame, `--precision max`, scalar path `--cpumask 4294967295` against `--cpumask 48` (AVX2), the default dispatch (AVX-512) and `adm_sycl` on the A380. `s0` is `integer_adm_scale0`. Limits of 1 and 100 gave identical output on every pair and every implementation.

| Input | AVX2, 1.2 | AVX-512, 1.2 | AVX2, 1.5 | AVX-512, 1.5 | SYCL, 1.2 | SYCL, 1.5 |
|---|---|---|---|---|---|---|
| `src01` 576x324, 48 frames | s0 1.23e-6, adm2 1.56e-7, aim 1.16e-7, adm3 8.83e-8 | s0 1.15e-6, adm2 1.46e-7, aim 1.14e-7, adm3 8.11e-8 | s0 3.07e-7, adm2 3.9e-8, aim 1.56e-7 | as AVX2 | s0 1.23e-6, adm2 1.55e-7, aim 2.38e-7, adm3 1.77e-7 | s0 3.07e-7, adm2 3.83e-8, aim 1.6e-7 |
| `src01` 10 / 12 / 16 bit | s0 9.08e-7, adm2 1.15e-7 | s0 8.33e-7, adm2 1.06e-7 | s0 3.02e-7 | as AVX2 | s0 1.12e-6, adm2 1.44e-7 | s0 3.02e-7 |
| Checkerboard 1080p, 1 px shift | identical | identical | s0 3.44e-5, adm2 3.58e-6, aim 2.07e-6, adm3 2.83e-6 | as AVX2 | s0 1.65e-5, adm2 1.71e-6, aim 1.4e-6 | s0 1.73e-5, adm2 1.8e-6 |
| Checkerboard 1080p, 10 px shift | identical | identical | identical | identical | identical | identical |
| `sparks` 480x270 10 bit | s0 2.77e-6, adm2 2.91e-7 | as AVX2 | s0 8.32e-7 | as AVX2 | s0 3.14e-6, adm2 3.3e-7 | s0 7.4e-7 |
| `akiyo` 352x288 | s0 6.23e-6, adm2 7.76e-7 | as AVX2 | s0 6.33e-7 | as AVX2 | s0 7.28e-6, adm2 9.07e-7 | s0 6.33e-7 |
| `akiyo` 18x22 | s0 8.88e-6, adm2 9.21e-7 | identical (scalar tail only) | s0 2.43e-7 | identical | s0 1.58e-5, adm2 1.64e-6 | s0 1.1e-6 |
| 160x90 | s0 1.82e-6, adm2 2.21e-7 | as AVX2 | s0 7.96e-7 | as AVX2 | s0 2.7e-6, adm2 3.34e-7 | s0 9.67e-7 |
| Independent noise 576x324 | s0 2.78e-7 | as AVX2 | s0 2.5e-7 | s0 2.22e-7 | s0 3.33e-7 | s0 2.5e-7 |
| Noise plus perturbation | s0 5.55e-7, adm2 1.47e-7 | s0 5.54e-7 | s0 1.11e-7 | as AVX2 | s0 7.77e-7, adm2 2.06e-7 | s0 1.11e-7 |
| Impulses on a gradient | aim 1.74e-6, adm3 8.68e-7 | aim 1.68e-6, adm3 8.41e-7 | aim 1.36e-7 | as AVX2 | aim 1.22e-6, adm3 6.11e-7 | aim 1.36e-6, adm3 6.78e-7 |
| Blurred blocks 576x324 | s0 3.56e-5, adm2 3.22e-6, adm3 1.61e-6 | s0 3.44e-5, adm2 3.12e-6, adm3 1.56e-6 | identical | identical | s0 3.6e-5, adm2 3.26e-6 | identical |
| Big Buck Bunny 3840x2160, 200 frames | s0 1.99e-6, adm2 2.89e-7, aim 3.94e-7, adm3 2.67e-7 | as AVX2 | s0 1.16e-6, adm2 2.05e-7 | as AVX2 | s0 2.31e-6, adm2 4.03e-7 | s0 9.54e-7, adm2 1.65e-7 |
| Flat, `KristenAndSara` (identical pictures), flat with patches (64x64, 24x24), 1 px stripes | identical | identical | identical | identical | identical | identical |

In the x86 comparisons scales 1 to 3 never differed in a score, although the AVX-512 scale 1-3 kernel rounded too: a difference of one in a 32-bit coefficient is almost always below the float the scale score is stored in (the SYCL twin shows 1.05e-7 on `integer_adm_scale2` for Big Buck Bunny at 1.5). At the kernel level it is visible: the comparison harness of pull request #1700 (14976 generated bands) reports 9025 differing cases for AVX2 and 8960 for AVX-512 at a limit of 1.2.

### After

Measured again on the change rebased onto master `b22ad4e1a`. Scalar against AVX2 and against AVX-512: identical JSON on all 20 pairs and on Big Buck Bunny (200 frames) at limits of 1, 1.2, 1.5 and 100. Scalar against `adm_sycl` on the A380: the same, 20 of 20 pairs and 200 of 200 Big Buck Bunny frames at every limit. The harness reports 0 differing cases of 14976 for both vector levels at 1.2.

Old against new binary at the default limit: with the default model plus `adm` and `float_adm` on the default dispatch, the 16 pairs the model accepts are identical (29 metrics each); with `adm` and `float_adm` alone, all 21 pairs (Big Buck Bunny included) are identical on the scalar path, on AVX2 and on AVX-512. The scalar path does not change at any limit.

Netflix golden gate (golden profile, gcc 16.2.1): 271 passed, 12 skipped, before and after. Three of its assertions use the integer extractor at a limit of 1.2, on the 352x288 `akiyo` pair with the default dispatch (`test_run_integer_adm_fextractor_akiyo_multiply_enhn_gain_limit_1d2` in `python/test/vmafexec_feature_extractor_test.py`, places=5). They hold with more margin than before, because the vector kernels now return the scalar's values:

| Assertion | Golden | Before (AVX-512) | After (every level) |
|---|---|---|---|
| `integer_ADM_feature_adm2_egl_1.2_score` | 1.116609 | 1.116610457808281 | 1.1166096821570612 |
| `integer_ADM_feature_aim_egl_1.2_score` | 0.01157 | 0.01156946734921909 | 0.01156964647206431 |
| `integer_ADM_feature_adm3_egl_1.2_score` | 1.05252 | 1.052520495229531 | 1.0525200178424985 |

### CUDA and HIP already truncate

`adm_decouple_inline.cuh` assigns the double product to an `int32_t`, which truncates. Compiled to PTX for sm_89, `adm_cm.cu` has 156 and `adm_csf.cu` 16 double-to-integer conversions, all `cvt.rzi.s32.f64`; none rounds to nearest. The conversion also saturates, which is what makes the scale 1-3 product safe at a limit of 100: a restored sample above 2^31 / 100 gives a product outside int32, the conversion yields `INT32_MAX` or `INT32_MIN`, and the minimum or maximum with `t` then picks `t`, as the scalar's double comparison does.

On the device, `adm_cuda` against the scalar differs by at most 2.56e-7 to 2.91e-7 on 19 of 21 pairs at every limit, 100 included (the largest, 2.91e-7, is `integer_adm_scale2` on Big Buck Bunny at a limit of 1). That distance comes from the twin's host finalisation and does not grow under a non-integer limit (blurred blocks: 4.35e-8 at 1.2, where the rounding kernels were 3.5e-5 off). `adm_hip` on gfx1036 is bit-identical to the scalar on 19 of 21 pairs at every limit; impulses on a gradient differ by 3.46e-7 (scales 2 and 3) at 1, 1.2 and 1.5 and by 3.96e-7 at 100, and Big Buck Bunny by 1.36e-7 on `integer_adm_scale0` at every limit, so those differences are not the limit's either. One HIP run of the 160x90 pair at a limit of 100 came back 4.8e-3 off on every scale and was identical on two repeats; that is the dropped dispatch of `T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01`, not this change (no HIP file is touched).

### The integer form of the double product

`adm_gain_limit_product()` writes the limit as `M * 2^-s` with `M` in [2^52, 2^53), takes `a = |rst| <= 2^31`, and forms `P = a * M` as `hi * 2^32 + lo` from two 64-bit products. The integer part is `N = P >> s` and the fraction `F = P mod 2^s`. `fl()` moves the exact product by at most half a unit in the last place, so the truncated double product is `N` or `N + 1`, and it is `N + 1` exactly when

```text
2^s - F <= 2^(s + b - 54)        b = bit length of N
```

(a tie rounds to `N + 1`, whose significand is even). Multiplying by `2^(54 - s)` gives `G <= 2^b` with `G = (2^s - F) * 2^(54 - s)`, which is `bitlen(G - 1) <= bitlen(N)`; and `bitlen(x) <= bitlen(y)` is `not (y < x and y < (x xor y))`. No bit scan, no 128-bit integer and no floating point are involved. An integral limit gives `F = 0`, so `G = 2^54` and the product never rounds.

A standalone check compared it with `(int64_t)((double)rst * gain)` on 106.8 million pairs: 18 limits (1, 1.2, 1.5, 100, the neighbours of 1, 64 and 100, 0.25, 2^21 - 0.5 and thirds) over every sample in [-300000, 300000], two million random int32 samples each and the ends of the range, then 3000 random limits in [1, 100) with 20000 samples each. No mismatch. `test_adm_gain_limit` keeps a reduced version of that sweep in the tree.

### Kernel scratch memory on the A380

`test_sycl_kernel_scratch` audits 108 kernels after the change: two use scratch memory, both listed in the ratchet (`float_adm_sycl`); the integer ADM kernels use none, as before.

### A second difference in the scale-0 angle test

The first version of the new vector test left 28 samples of a 24x12 band different at a limit of 1.2 after the conversions were fixed. Each had `h = v = -32768` in the distorted picture. `_mm256_madd_epi16` forms `h * h + v * v` in int32; with both products 2^30 the sum is 2^31 and the lane holds `INT32_MIN`. The scalar forms the sum in int64. The negative magnitude made the right-hand side of the angle predicate negative and set the flag.

The int32 sum can wrap only in that one way: the most negative true sum is `2 * (-32768 * 32767)`, above `INT32_MIN`. Reading a lane of `INT32_MIN` as 2^31 is therefore always right. The squared magnitudes are read as unsigned; the dot product, which is signed, is replaced where it is `INT32_MIN`.

A decoded picture cannot produce the operand. The scale-0 bands are the 8-bit-normalised DWT in Q6: the largest magnitude of the h, v or d band is `255 * (sum of |hi (x) lo|) / 2 * 64`, about 22850 (22930 for 16-bit input). The test bands reach the bottom of the int16 range on purpose.

### Planted defects

Each of the following, applied alone, makes `test_adm_decouple_matches_scalar_for_gains` fail:

| Defect | Samples of a 24x12 band reported at a limit of 1.2 |
|---|---|
| `_mm256_cvtpd_epi32` in `decouple_gain_avx2()` | 304 scale-0 |
| `_mm512_cvtpd_epi32` in `decouple_gain_avx512()` | 204 scale-0 |
| `_mm512_cvtpd_epi64` in `decouple_s123_limit_half_avx512()` | 216 scale 1-3 |
| AVX2 squared magnitudes read as int32 | 28 scale-0 |
| AVX2 dot product not corrected at `INT32_MIN` | 4 scale-0 |
| AVX-512 squared magnitudes converted as signed | 28 scale-0 |
| AVX-512 dot product not corrected at `INT32_MIN` | 4 scale-0 |

On the unchanged base the test reports 334 scale-0 samples for AVX2 and 234 scale-0 plus 226 scale 1-3 samples for AVX-512.

`test_gpu_adm_fractional_gain_limit_parity` on the A380 fails on the base with `integer_adm_scale0` 0.90504173 against the CPU's 0.90482954 (96x64, limit 1.2, a picture whose contrast the distorted copy doubles) and passes bit for bit after the change. It also runs on CUDA and HIP at the places=4 gate.

### Timings

x86 decouple stages, harness of pull request #1700 (`0fda8066a` against this change, gcc 16.2.1 `-O3`, Ryzen 9 9950X3D, minimum of 6 runs of 60 frames, load average 6 to 8), ms per frame:

| Level | Geometry | Scale 0 before | Scale 0 after | Scales 1-3 before | Scales 1-3 after |
|---|---|---|---|---|---|
| AVX2 | 1920x1080 8 bit | 2.503 | 2.493 | 1.378 | 1.371 |
| AVX2 | 1920x1080 10 bit | 2.524 | 2.490 | 1.381 | 1.401 |
| AVX2 | 576x324 8 bit | 0.119 | 0.121 | 0.091 | 0.091 |
| AVX-512 | 1920x1080 8 bit | 0.499 | 0.504 | 0.856 | 0.871 |
| AVX-512 | 1920x1080 10 bit | 0.503 | 0.507 | 0.878 | 0.884 |
| AVX-512 | 576x324 8 bit | 0.052 | 0.052 | 0.045 | 0.044 |

Every pair is within 2%, which is the spread between repeated runs on that host. A truncating conversion costs what a rounding one does; the angle test gains one compare and one masked conversion per sixteen samples on AVX-512 and one compare per sample on AVX2.

SYCL on the Arc A380, `vmaf --feature adm_sycl --backend sycl` on Big Buck Bunny 3840x2160 (200 frames, wall time of the whole process divided by 200, median of 5, load average 6):

| Limit | Before | After |
|---|---|---|
| 100 (default) | 10.88 ms/frame | 11.04 ms/frame |
| 1.2 | 10.93 ms/frame | 11.11 ms/frame |

The integer product costs about 0.17 ms per 4K frame (1.5%) against the Q31 product it replaces: a magnitude and a sign restore, two variable shifts and a comparison more per limited sample.

## Alternatives explored

See the decision matrix of [ADR-1413](../adr/1413-adm-gain-limit-truncated-double-product.md). Two SYCL forms were tried on the host before the comparison form: Q31 with truncation of the magnitude, which still gives 5 for `5 * 1.2`, and a literal round-to-53-bits with a bit scan of the 85-bit product, which is correct and longer.

## Open questions

- The Metal twin multiplies in binary32 (`T-METAL-ADM-GAIN-LIMIT-FLOAT32-2026-10-01`). `adm_gain_limit_product()` uses only 64-bit integer operations that MSL has, so a port is mechanical, but it needs an Apple device to verify.
- `adm_cuda` is up to 2.9e-7 from the scalar at every limit because of its host finalisation. The SYCL twin removed the same distance in ADR-1362.
- `adm_hip` differs from the scalar on impulses on a gradient at scales 2 and 3 (up to 3.96e-7) and on Big Buck Bunny at scale 0 (1.36e-7), at every limit. Reported to the HIP workstream; not changed here.

## Related

- [ADR-1413](../adr/1413-adm-gain-limit-truncated-double-product.md), [ADR-1402](../adr/1402-adm-cm-centre-tap-int32.md), [ADR-1362](../adr/1362-sycl-integer-adm-aim-device-pass.md), [ADR-0220](../adr/0220-sycl-fp64-fallback.md).
- `docs/state.md`: `T-ADM-DECOUPLE-X86-FRACTIONAL-GAIN-ROUNDING-2026-10-01`, `T-SYCL-ADM-FRACTIONAL-GAIN-LIMIT-2026-09-29`, `T-METAL-ADM-GAIN-LIMIT-FLOAT32-2026-10-01`.

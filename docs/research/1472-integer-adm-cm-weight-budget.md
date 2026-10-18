<!-- markdownlint-disable MD013 MD060 -->
# Research-1472: Which term of the integer ADM contrast-masking reduction wraps in Barten mode, and what weight keeps it in range

- **Status**: Active
- **Workstream**: [ADR-1472](../adr/1472-integer-adm-cm-weight-budget.md), [ADR-1325](../adr/1325-integer-adm-barten-fixed-point-normalization.md)
- **Last updated**: 2026-10-02

## Question

`vmaf --feature adm=adm_csf_mode=1` fails every frame of the 10 px
checkerboard with `aim_num=-nan`. Which operation wraps, at which scale, is it
the defect of the open row `T-ADM-AIM-BARTEN-SCALE-TERM-WRAP-2026-10-01`, does
upstream have it, and what is the widest weight that cannot wrap on any
picture?

## Sources

- `core/src/feature/integer_adm_kernels.h` (`i4_adm_cm_scale()`,
  `i4_adm_cm_accum_round()`, `adm_cm_accum_round()`, `i4_adm_cm_thresh()`,
  `i4_dwt2_round()`), `core/src/feature/adm_csf_fixed_point.h`,
  `core/src/feature/integer_adm.h` (wavelet taps), at master `ade338374`.
- Upstream Netflix/vmaf `cea2b4d83` (2026-10-01),
  `libvmaf/src/feature/integer_adm.c`, built with
  `meson setup build -Denable_float=true`.
- Reference workstation: Ryzen 9 9950X3D, RTX 4090, gfx1036, Arc A380 (xe), gcc 16.2.1,
  icx 2026.0. CPU build:
  `meson setup build-cpu core -Denable_cuda=false -Denable_sycl=false
  -Denable_hip=false -Db_lto=false --buildtype release`.
- Fixtures: both 1080p checkerboard pairs, the Netflix 576x324 pair (48
  frames), and 512x512 frames built for this question (below).

## Findings

### The term

An instrumented build compared every weighted sample, square and cube of
`i4_adm_cm_accum_px()` with the same value in int64. On the 10 px
checkerboard with `adm_csf_mode=1` the first difference is

```text
scale 3, diagonal band, (8, 13), band 120x68:
  coefficient 460572333, weight 905160448 (exponent 7)
  weighted    1553043199        threshold -27
  excess      1553043226        (excess^2 + 2^29) >> 30 = 2246297208  > INT32_MAX
```

The weighted sample fits int32; its square, narrowed by
`v_sq = (int32_t)(...)`, does not. The cube of that sample is negative, the
row and frame accumulators of the band follow, and `powf()` of a negative
base with exponent 1/3 is NaN. The reduction is used for both numerators: the
additive-impairment one (`aim_num`, source `decouple_a`) here, the detail-loss
one on other pictures.

The open row's reproducer (1 px checkerboard, `adm_csf_scale=1.2`) is the same
operation at scale 1 (vertical band, square 2787428024). The row is this
defect.

A wrap does not always end in NaN. On the 1 px checkerboard with
`adm_csf_mode=1` alone one scale-3 square wraps (2181864011), the accumulator
stays positive, and the extractor returns `integer_adm2` 0.587102 for frame 0
with no message. `float_adm` gives 0.783557; the fixed extractor gives
0.783548.

### The budget

`v_sq` fits int32 for an excess up to 1518500249 (shift 30: scales 1 to 3 and
the scale-0 diagonal band) and up to 2^30 - 1 (shift 29: scale-0 horizontal
and vertical). The excess is the weighted coefficient minus the threshold; at
scales 1 to 3 the threshold is at least -27 (27 terms of at least -1, because
ADR-0155 keeps upstream's `1u << 31` rounding term in an int32).

The largest coefficient: a detail band is linear in the centred pixel, so its
magnitude is at most half the absolute sum of the composite filter (the
low-pass filter at each earlier level, the band's filter at its own level,
mirrored as the index tables mirror), in the band's fixed-point format.

| Scale | Format | Absolute sum, low-pass / high-pass (1-D) | Bound h, v | Bound d | Reached, 8 bit |
|---|---|---|---|---|---|
| 0 | 2^14 | 1.6730 / 1.6730 | 22929 | 22929 | 22840 |
| 1 | 2^29 | 2.3571 / 2.2901 | 1448980000 | 1407800000 | 1443331337 (h), 1402284938 (d) |
| 2 | 2^27 | 3.3410 / 3.3464 | 750300000 | 751508000 | 747375525 (h), 748575616 (d) |
| 3 | 2^26 | 4.7235 / 4.6848 | 742509000 | 736430000 | 739619691 (h), 733560527 (d) |

"Reached" is the largest coefficient the instrumented build saw on a frame
whose luma is 255 where the composite filter of the centre coefficient is
positive and 0 where it is negative (`core/test/test_integer_adm_cm_budget.c`
builds the same frames). It is 255/256 of the bound, as an 8-bit picture
should be.

Dividing the excess budget by the bound gives the weight limits of ADR-1472.
The Watson97 weights at the default geometry are 36453 (scale 0), 137373760,
186284176 and 196165808 (scales 1 to 3, horizontal and vertical): all under
the limits, and Watson97 weights only fall as the viewing geometry grows, so
upstream's default path was sized correctly. The blend modes' weights are of
the same size. Only Barten mode reaches the limits.

### Before and after

`adm=debug=true:<options>`, `--precision max`, first frame unless noted.

| Input | Options | master `ade338374` | fixed | `float_adm` |
|---|---|---|---|---|
| 10 px checkerboard | `adm_csf_mode=1` | fails, `aim_num=-nan` | `integer_aim` 0.999874302, `integer_adm2` 0.000125263 | 0.999874855, 0.000125272 |
| 10 px checkerboard | `adm_csf_mode=1:adm_csf_scale=1.2` | fails | `integer_aim` 0.999884629 | 0.999885458 |
| 1 px checkerboard, frames 0, 1, 2 | `adm_csf_mode=1` | `integer_adm2` 0.587101992, 0.663831720, 0.563360118 | 0.783547840, 0.834673433, 0.784913246 | 0.783557289, 0.834681833, 0.784922459 |
| 1 px checkerboard | `adm_csf_mode=1:adm_csf_scale=1.2` | fails | `integer_adm2` 0.782465110 | 0.782474882 |
| Netflix pair, 48 frames | `adm_csf_mode=1` | correct | largest change: `integer_adm2` 2.1e-7, `integer_adm3` 4.1e-7, `integer_aim` 7.7e-7 | |
| Netflix pair, 48 frames | default, `adm_csf_mode=2`, `adm_csf_mode=3` | | all 18 outputs identical | |

Adversarial 512x512 frames (reference of opposite polarity), `adm_csf_mode=1`:
of the six frames for scales 1 to 3 master fails on five and returns
`integer_aim` 0.8645 for 1.0037 on the sixth (the two scale-0 frames score on
master at the default Barten scales; with `adm_csf_scale=1.4` and
`adm_csf_diag_scale=0.3` the scale-0 horizontal weight is 55532 and master
fails there too). The fixed extractor agrees with `float_adm` to 8.7e-6
(`aim`) and 7.2e-7 (`adm2`) wherever `float_adm` does not clip `aim` at 1.

The record of standards batch B1 (every `adm` and `float_adm` output under 21
option sets, 20 fixtures, scalar / AVX2 / AVX-512): 1635 of 1695 cases
identical to master; the 60 that differ are the integer `adm_csf_mode=1` set.

### Twins

`adm_cuda` (RTX 4090), the HIP twin (gfx1036) and the SYCL twin (Arc A380)
against `--backend cpu`, all 18 outputs of `adm=debug=true`: identical under
the default options,
`adm_csf_mode=1`, `adm_csf_mode=1:adm_csf_scale=1.2`,
`adm_csf_mode=1:adm_csf_scale=1.4:adm_csf_diag_scale=0.3` and `adm_csf_mode=2`
on the Netflix pair (48 frames) and both checkerboards (3 frames each), and on
six adversarial frames. No kernel changed: the twins take the weights and the
exponent from `adm_csf_fixed_scale()`.

### Upstream

Upstream has the option (`integer_adm.c:179`) and the same reduction
(`I4_ADM_CM_ACCUM_ROUND`, `integer_adm.c:698`), and converts the weights with
`(uint16_t)(rfactor1[0] * pow2_21)` / `(uint32_t)(rfactor1[0] * pow(2, 32))`
without any normalisation. Barten weights (1.21 at scale 0 to 26.98 at scale
3) do not fit either type. Measured with upstream's binary:

| Input, `adm=adm_csf_mode=1` | upstream `integer_adm2` / `integer_aim` | upstream `float_adm` |
|---|---|---|
| Netflix pair, frame 0 | 0.002706 / 0.000147 | 0.965404 / 0.006402 |
| 1 px checkerboard | NaN (`null` in the JSON) / 0.001413 | 0.783557 / 0.136156 |
| 10 px checkerboard | 0.000125 / NaN | 0.000125 / 0.999875 |

So upstream's fixed-point Barten mode is wrong on ordinary content too (the
fork's ADR-1191 / ADR-1325 history). For upstream to follow it needs the
fork's shared-exponent normalisation with the limits of ADR-1472.

### Cost

BBB 3840x2160, 20 frames, one thread, median of five runs at a load average
of 45: default `adm` 29.99 ms per frame before and 29.13 after;
`adm_csf_mode=1` 64.47 before and 65.06 after. No change: the exponent is
chosen once at initialisation.

## Open questions

- The AVX2 / AVX-512 weighted-CSF and contrast-masking kernels still run only
  when every exponent is 0 (ADR-1325). Carrying the exponent through them is
  performance work.
- `integer_aim` is not clipped at 1 where `float_adm` clips (1.0037 against
  1 on an adversarial frame); `core/test/test_integer_adm_aim_unclipped.c`
  holds that behaviour. Not part of this question.

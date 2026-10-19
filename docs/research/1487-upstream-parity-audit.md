<!-- markdownlint-disable MD013 MD060 -->
# Research-1487: Where the fork's CPU extractors differ from Netflix/vmaf, and why

- **Status**: Active
- **Workstream**: [ADR-1487](../adr/1487-upstream-parity-policy-and-guard.md), [ADR-1479](../adr/1479-ciede-422-chroma-subsampling-flags.md) to [ADR-1486](../adr/1486-float-motion-scale1-uses-callers-stride.md)
- **Last updated**: 2026-10-03

## Question

The fork inherited its CPU extractors from Netflix/vmaf. For every extractor,
option and model both trees have: is the fork's output Netflix's, bit for
bit? Where it is not, which fork change made the difference, and was it meant?

## Sources

- Netflix/vmaf `cea2b4d8` (2026-10-01); fork master `b34732ae1` for the audit,
  `d6719babf` for the guard's measurements. Netflix `9e48141b` adds an
  aarch64-only kernel; its x86 arithmetic is `cea2b4d8`'s.
- Reference workstation: Ryzen 9 9950X3D (AVX-512), GCC 16.2.1, glibc 2.44.
  Upstream `-O3 -std=c11`; fork `-O3 -std=c23 -ffp-contract=off`. Neither
  `libvmaf` has a fused multiply-add outside its `*_avx2` / `*_avx512`
  functions.
- A harness on the C API that prints every value of the feature collector at
  `%.17g`, built against each tree (now `scripts/dev/upstream_parity_harness.c`).
  Audit: 16 shared extractors, 31 fixtures, 185 option variants, 25 model
  files, scalar (`cpumask` 63), AVX2 (48) and default dispatch; 348,132 values
  per dispatch.
- Bisection by building: 685 first-parent fork commits, 271 builds, 68 commits
  at which a probe value changed.
- Two scratch trees: the fork with the unintended differences reverted, and
  that tree with two deliberate deviations undone as well.
- The guard's own measurements: on the host above (2026-10-02), then in the
  dev container image `sha256:43ef1e32cb32` (GCC 15.2.0, glibc 2.43) on the
  same machine (2026-10-03), which is where the guard now runs.

## Findings

### Size

| | Identical values of 348,132 (scalar) |
| --- | --- |
| Fork as it was | 177,788 |
| With the six unintended differences reverted | 315,257 |
| With two deliberate deviations also undone (`float_adm` division, `float_ms_ssim` decimation and magnitude) | 343,954 |

The remaining 4,178 values fall in the deliberate classes below. Prediction,
SVM and pooling arithmetic are identical: every model score difference is a
feature difference. `cambi`, `vif`, `float_vif`, `motion`, `float_motion`,
`float_psnr`, `float_moment`, `psnr_y` and `float_ssim` are identical at their
default options wherever both trees run.

### Unintended differences

Each was confirmed by restoring upstream's expression in a scratch build and
seeing the metric become identical. None was found by a gate: the Netflix
golden assertions hold with all six reverted and with none.

| Extractor | The fork's form | From | Size |
| --- | --- | --- | --- |
| integer `adm` | `params->k * (double)temp * temp` in the DWT quantisation step | #552 (CodeQL cleanup, 2026-05-09) | `adm2` 8.5e-8; `vmaf_v0.6.1` 1.8e-5 |
| `float_adm` | `double` intermediates in `dwt_quant_step()` | #760 (port) | `adm2` 1.1e-7; float models 2.2e-5 |
| `float_adm`, `adm_csf_mode=1` | casts in `barten_csf_tools.h` | #44 (port) | 0.23 on the debug sums |
| `ciede` | `(double)` in two expressions of `ciede2000()` | #552 | 1e-8 |
| `psnr_hvs` | `(double)s_mask * s_gvar` in the masking threshold | #552 | 7.9e-7 dB |
| `speed_chroma`, `speed_temporal` | `log2f`, `1.0f / sqrtf`, `0.75f` where upstream computes in `double` | #213 (port) | `speed_chroma` 2.3e-5; `vmaf_v1.0.16` models 2.5e-5 |

Of 127 lint, cleanup and refactor commits among the candidates, only #552
changed arithmetic. The three port commits introduced their differences while
porting; they were found by comparing sources, not by bisection.

### Deliberate differences

| Difference | Size | Recorded by | Upstream |
| --- | --- | --- | --- |
| `float_adm` divides in the decouple | `adm2` 6.8e-8; float models 1.5e-5 | ADR-1442 | Netflix/vmaf#1655, #1654 |
| `float_ms_ssim` separable decimation | 8e-8 typical, 1.8e-6 at most | ADR-0125, bound by ADR-1487 | none |
| `float_ms_ssim` magnitude before `pow()` | NaN upstream on the 10 px checkerboard | ADR-1033, ADR-1484 | #1665 |
| `apsnr` of a plane without error | 114 against 60 dB | ADR-1033, ADR-1485 | #1666 |
| `float_motion` scale-1 stride | up to 25 | ADR-1033, ADR-1486 | #1667 |
| integer `adm` scale-0 masking centre tap | noise only: `adm2` 9.4e-5 | ADR-1402 | #1602, #1608, #1610 |
| integer `adm` gain-limit truncation on SIMD | 5.7e-5 at `adm_enhn_gain_limit=1.2` | ADR-1413 | #1633 |
| integer `adm` Barten range, CSF rejection | upstream wraps (NaN) | ADR-1191, ADR-1325, ADR-1472 | #1662 |
| `ciede` 4:2:2 chroma flags | 0.153 on 48 of 48 frames | ADR-1479 | #1611 (another contributor) |
| `speed_temporal` applies `speed_max_val` | up to 75 | ADR-1301 | #1668 |
| `speed_temporal` buffers at `speed_prescale` above 1 | up to 195; upstream overruns its buffers | ADR-1480 | #1627, #1626 |
| integer `adm` on frames of 17 to 32 pixels | scale 3 up to 0.23 | ADR-1482 | #1599, #1600 |
| a failing extractor fails the run | status only | ADR-1481 | none propagates the error |
| odd-sized chroma planes round up | `psnr_cb` / `psnr_cr` 0.83 dB, `ciede` 0.2 | ADR-1483 | none (#1604 covers the tools) |
| `psnr_hvs` scores luma on 4:0:0 | a name, not a value | ADR-1487 | none |
| `ciede` squares as products | 5.2e-12 on glibc 2.44; none in the dev image | ADR-1467 | none |
| harmonic-mean pool of an all-infinite or NaN metric is 0 | `inf` or NaN upstream | ADR-1008 | none |

The last two were not in the audit: ADR-1467 landed after its base, and the
pooling guard shows only where the guard compares pooled values of a metric
whose every frame is not finite (`psnr_hvs` chroma on identical planes).

### Missing port

`motion_five_frame_window` (upstream `a2b59b77`, `a4a1492d`): the fork returns
`-ENOTSUP` (ADR-0337), so the four `vmaf_v1.0.16_hfr` models cannot score.

### A defect found on the way

`float_ssim` on a frame smaller than its 11x11 window returned heap garbage on
the fork's SIMD paths (`T-FLOAT-SSIM-SUB-WINDOW-SIMD-COUNT-2026-10-02`,
fork PR #1882).

### The environment and undefined values

Two things the audit's host hid came out of the guard's runs.

Upstream's value depends on the C library. Its `ciede2000()` squares with
`powf(x, 2)`. On the host (glibc 2.44) that differs from the fork's product
on 119 of 327 frames, up to 2.2e-11 (fork PR #1892); in the dev image
(glibc 2.43) it differs on none of the guard's frames. The guard therefore
compares in the image only.

Some upstream outputs are undefined. The full matrix was repeated on the host
four times, twice with the heap unfilled and with `MALLOC_PERTURB_=85` and
`=170`: 48 upstream runs changed (`ciede` on 12x9, 17x17 and 19x19;
`float_motion` with `motion_add_scale1` and `motion_add_uv` on four
fixtures; `float_adm` and `float_vif` on 12x9 and 8x8; `speed_temporal` at
`speed_prescale=2`, which sometimes crashes), and no run of the fork did. In
the image the guard's heap check finds 1,675 upstream outputs in 45 runs and
none of the fork's. At 12x9 and 8x8 upstream's `float_adm` `adm2` moves from
3.94 to 3.38 and from 1.42 to 3.6e-11 with the fill. Where upstream's value
is undefined the guard's bound is `inf`.

## What was not compared

aarch64 and NEON; clang, icx and MSVC; any C library but glibc 2.44 (the
audit) and 2.43 (the image); upstream's CUDA; the Python layer; the
command-line output; more than one thread; `n_subsample`; pooling beyond mean
and harmonic mean; the 15 extractors only the fork has; `float_ansnr`, which
only upstream has. Integer ADM in Barten mode and on synthetic noise is
compared, but upstream's output there is wrapped or differs between its own
scalar and SIMD paths, so an unintended difference could hide under the
deliberate ones.

## Consequence

The rule of ADR-1487 (Netflix's source is the reference, a deviation needs an
ADR) and a guard that repeats this comparison: `make upstream-parity`,
described in [upstream parity](../development/upstream-parity.md).

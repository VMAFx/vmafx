<!-- markdownlint-disable MD013 MD060 -->
# Research-2132: CAMBI on frames shorter or narrower than its window

- **Status**: Active
- **Workstream**: [ADR-1393](../adr/1393-cambi-clip-window-to-frame.md)
- **Last updated**: 2026-09-30

## Question

Netflix/vmaf#1628 reports that CPU `cambi` reads and writes outside its buffers on wide, short frames. Which frame sizes are affected, in which of the fork's three c-values walks, does the same defect exist for columns, how do scores move when the walks stop at the frame, and what do the GPU twins do on the same sizes?

## Sources

- `core/src/feature/cambi.c`: `adjust_window_size()`, `decimate()` (in place), `c_values_first_pass()`, `c_values_top_edge()`, `c_values_middle_slide()`, `c_values_bottom_edge()`, `vmaf_cambi_calculate_c_values()` (the host walk the CUDA, HIP and Metal twins call).
- `core/src/feature/x86/cambi_avx2.c`: `calculate_c_values_avx2()` and its `_avx2` phase helpers (upstream mirror, built and tested, not dispatched).
- `core/src/feature/cambi_c_values_frame.h`: `cambi_calculate_c_values_frame()`, the walk of the dispatched AVX2 scan, AVX-512 and NEON drivers; `cambi_c_values_row_step()` visits columns `j0 < width` only.
- `core/src/feature/sycl/integer_cambi_sycl.cpp`: `cvals_prime()` and `cvals_column()`, the device walk.
- [Netflix/vmaf#1628](https://github.com/Netflix/vmaf/issues/1628) and [Netflix/vmaf#1629](https://github.com/Netflix/vmaf/pull/1629).

## Findings

### Which frames are affected

The window is `((window_size * (width + height)) / 375) >> 4`, made odd, and is the same at all five scales; `pad_size` is half of it. Each scale halves the previous one, rounding up, so the coarsest has `ceil(n / 16)` rows and columns.

- **Short frames.** The first pass reads rows `0 .. pad_size - 1`, the top edge writes c-values rows `0 .. pad_size`, the bottom edge starts at `height - pad_size`. With at most `pad_size` rows at the coarsest scale, a row outside the frame is written; with fewer than `pad_size`, rows outside are also read into in-frame histograms. With the default `window_size` of 65: up to 176 rows at 1920 wide (window 23, `pad_size` 11), 240 at 2560 (31, 15) and 352 at 3840 (45, 22) are out-of-bounds; up to 160, 224 and 336 rows also change scores.
- **Narrow frames.** The scalar and AVX2-mirror walks run their first column loop for `j < pad_size`. With fewer than `pad_size` columns at a scale they add columns `width .. pad_size - 1` of the same rows. `decimate()` writes each scale into the top-left of the previous one, so those columns hold the previous scale's pixels: in bounds, invisible to ASan, and part of the score. The shared SIMD walk never visits them. With the default window: widths up to 80 at 1080 high (window 13, `pad_size` 6), 160 at 1920 high and 176 at 2160 high.

### Scores before and after

Inputs: 3 frames of 8-bit 4:2:0, luma `vramp = 16 + 200 * y // h`, `hramp = 16 + x // 24 + y // 97`, `diag = 40 + (x + 2 * y) // 31`, `noise` = uniform 16..235 (seeded); the distorted frame adds `(x // 37 + y // 29) % 2` (ramps) or uniform -2..2 (noise); chroma 128. Builds: master `10f27efe2` and the branch, clang 22 release, same flags. `--precision max`, `--cpumask 0` (AVX-512), `48` (AVX2), `63` (C); `cambi` and `cambi=full_ref=true`.

| Input | master | branch |
|---|---|---|
| 64x1920, 128x1920 (`vramp`, `hramp`, `diag`) | AVX-512 = AVX2; C differs (64x1920 `vramp` 14.964394451743877 vs 14.975700714938673) | all three levels equal the master AVX-512 score |
| 64x1920, 128x1920 `noise`; 96x1080 (6 columns, `pad_size` 6) all four | levels equal | identical to master |
| 1920x64, 1920x160, 3840x256 (all four inputs) | AVX-512 and AVX2 abort or segfault (rc -6 / -11); C completes on most | all three levels equal, exit 0 |
| 1920x176, 2560x240, 3840x352 (`hramp`, `diag`; exactly `pad_size` rows; levels 0 and 63 run) | score | identical to master |
| 2560x224 (`hramp`, `diag`; levels 0 and 63 run) | aborts at both levels (rc -6) | scores, e.g. `hramp` 20.110688509733645 at both levels |
| 3840x336 (`hramp`, `diag`; levels 0 and 63 run) | AVX-512 aborts; C completes (`hramp` 16.263081637901482) | `hramp` 16.26283611591702 at both levels |
| 1920x1080 ramps; Netflix `src01_hrc00/01_576x324`; checkerboard 1-px | score | identical to master at every level |

Wide short frames with fewer than `pad_size` rows that completed on master's C path change score; the largest change measured is 0.032, on 3840x128 with the horizontal ramp `16 + 60 * x / w` (distorted frame one level brighter) from the review of PR #1642: 19.544346510264372 to 19.512268984993305. Tall narrow frames change on the C path only, to the SIMD score.

A second set of narrow-frame runs (the fork-bugs track, same day, same host; gcc 16 static release, no LTO; before = `06c9a436a`, after = the column bound) used a vertical ramp `16 + 200 * y / h` with the distorted frame one level brighter, and a diagonal ramp `16 + 100 * y / h + 20 * x / w`. On the C path (`--cpumask 63`) before and after; AVX2 and AVX-512 equal the after value both times:

| Input | vertical ramp | diagonal ramp |
|---|---|---|
| 64x1920 | 16.14104577967309 -> 16.131541053198784 | 2.582976805284252 -> 2.578650847733566 |
| 128x1920 | 16.204769544358193 -> 16.201630948002265 | 11.99373373463734 -> 11.98747996233186 |
| 32x1080 | 14.836211339752843 -> 14.822459595556436 | 0 both |
| 216x3840 | 16.75343563505782 -> 16.75036849178967 | 10.86262642747679 -> 10.855079910796368 |

With `cambi=full_ref=true`, `cambi_source` moves by the same amount (64x1920 16.197054612107948 -> 16.18754988563364) and `cambi_full_reference` stays 0. Horizontal ramps, 8-level steps and noise at these four sizes are identical before and after at `--cpumask` 0, 48 and 63. Controls identical before and after at 0 and 63: the Netflix 576x324 pair (0.2596841837459806), both checkerboard pairs (0), 24 frames of BBB 3840x2160 (0.3258154750903016; `full_ref` 0.09068875268864279, source 0.2351267224016589).

### Unit level

`test_calculate_c_values_short_frame` (heights 1 to 10, window 9) and `test_calculate_c_values_narrow_frame` (widths 1 to 6, height 10, window 9) in `core/test/test_cambi.c` run every driver the host has against a from-scratch clipped-window histogram and check that nothing outside the frame is written. With the column loops unbounded, the narrow test fails on the scalar walk at one column, and on the AVX2 mirror when only its loops are left unbounded; with the row bounds removed, the short test fails for 1 to 4 rows on all five drivers.

### GPU twins

The CUDA, HIP and Metal twins call `vmaf_cambi_calculate_c_values()`, the scalar walk, on the host, so they take both bounds. The SYCL twin's device walk clamps every window to rows `[0, height - 1]` and columns `[0, width - 1]` (`cvals_prime()`, `cvals_column()`), so it already computed the clipped window. The fork-bugs runs above also scored `cambi_cuda` on the RTX 4090 and `cambi_hip` on the gfx1036 (`--backend hip --feature cambi_hip`; the name alone is refused under ADR-0498) at the four vertical-ramp sizes: before the change both gave exactly the old C-path value, after it exactly the new one, bit-identical to the CPU's default dispatch. The CUDA device c-values kernel in PR #1639 and `cvals_column()` of the SYCL twin clamp the column range to `[0, width)` by code reading; the SYCL twin is not measured here, because the Arc A380 on this host runs the xe kernel driver this boot, under which SYCL kernels that use scratch memory return wrong values.

### Lint note: the non-const picture of the c-values callback

`calculate_c_values_scan_avx2()` only reads `pic`, and cppcheck's `constParameterPointer` says so. Its type is fixed by `VmafCalcCValues`, which is upstream's callback type verbatim (`typedef void (*VmafCalcCValues)(VmafPicture *pic, const VmafPicture *mask_pic, ...)` in upstream `libvmaf/src/feature/cambi.c`; upstream's `calculate_c_values_avx2()` in `x86/cambi_avx2.h` matches it). Constifying it would move the scalar walk, the upstream-mirror AVX2 walk and their headers away from upstream for every sync, so the driver carries a cited inline suppression instead, as the VIF SIMD drivers do for their callback type (Research-2046).

## Alternatives explored

Rejecting such frames in `init()` (the alternative in Netflix/vmaf#1628), porting only the row bounds, and making the SIMD walks read the stale columns too; the comparison is in [ADR-1393](../adr/1393-cambi-clip-window-to-frame.md).

## Open questions

- Upstream has neither the column bound nor, until #1629 merges, the row bounds; its C-path scores for these sizes stay as the master column of the table.

## Related

- [ADR-1393](../adr/1393-cambi-clip-window-to-frame.md); `docs/state.md` rows `T-CAMBI-SHORT-FRAME-OOB-2026-09-30`, `T-CAMBI-NARROW-FRAME-COLUMNS-2026-09-30`.
- [CAMBI frame sizes](../metrics/cambi.md#frame-sizes).

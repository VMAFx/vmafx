<!-- markdownlint-disable MD013 MD060 -->
# Research-2127: CPU options on the SYCL PSNR, SSIM and float-motion twins — 2026-09-29

- **Status**: Active
- **Workstream**: RC3 SYCL, `T-BUG048-GPU-OPTION-PARITY-REMAINDER-2026-09-26` (SYCL part); decision in [ADR-1365](../adr/1365-sycl-twin-cpu-option-parity.md)
- **Last updated**: 2026-09-29

## Question

The SYCL twins `psnr_sycl`, `integer_ssim_sycl`, `float_ssim_sycl` and `float_motion_sycl` lacked CPU options (`min_sse`, `enable_mse`, `reduced_hbd_peak`, `enable_apsnr`; `enable_db`, `clip_db`, `enable_lcs`; `motion_max_val`). Where does each option act, can it be applied from what the device already reduces, and how close to `--backend cpu` does each get on real hardware?

## Sources

- CPU references: `core/src/feature/integer_psnr.c`, `integer_ssim.c`, `float_ssim.c` + `ssim.c` + `iqa/ssim_tools.c`, `float_motion.c`.
- Twins: `core/src/feature/sycl/integer_psnr_sycl.cpp`, `integer_ssim_sycl.cpp`, `float_motion_sycl.cpp`; the Metal `float_ssim_metal.mm` L/C/S reduction for comparison.
- Toolchain: oneAPI DPC++ 2026.1 (`icpx`), AOT for `bmg-g21,adl-s`, `vmaf-dev-mcp:ocloc`.
- Host: i9-12900K; Arc B580 (Level Zero device 0) and UHD 770 (device 1) through WSL2 `/dev/dxg`.

## Findings

### Where each option acts

| Option | CPU effect | Needs per-pixel data? |
|---|---|---|
| `min_sse`, `reduced_hbd_peak`, `enable_mse`, `enable_apsnr` | peak and `psnr_max` at init; per-plane MSE and PSNR from the frame SSE; SSE and sample-count totals summed over the clip | No: the twin already reduces the SSE per plane |
| `enable_db`, `clip_db` | `-10 log10(1 - ssim)` and a size-derived ceiling, on the pooled score | No |
| `enable_lcs` | means of the per-pixel L, C and S terms | Yes |
| `motion_max_val` | `MIN(score * fps_weight, max)` on each emitted score | No |

So only `enable_lcs` needs device work: a second vertical-pass kernel that computes L, C and S from the moments it already has and reduces three more per-work-group partials. Everything else is host arithmetic on values the device already hands back once per frame.

### Identical frames and the dB form

With `enable_db` the CPU reports `+inf` for a perfect score, or the `clip_db` ceiling (ADR-1221). Both CPU paths give exactly 1 for identical frames: `integer_ssim.c` computes in exact-integer doubles, and `iqa_ssim` sums identical l, c, s = 1 terms. The fp32 twins did not. In `integer_ssim_sycl`, the denominator's second factor `x2*w - mx^2 + y2*w - my^2 + c2` rounds differently from the numerator's `2*(xy*w - mxmy) + c2`, and icpx contracts `a*b + c` inside one expression, so a numerator and denominator that are mathematically equal differ in the last bits. With a residue of 1e-7 planted in the ratio, identical 161x91 frames score 69.2 dB on the device against the CPU's 93 dB ceiling. Holding each product in a named temporary, summing the two variances as a pair and returning 1 on `numerator == denominator` makes the two sides the same operations mirrored; identical windows then score exactly 1. The default linear scores move by at most 1.05e-8 against the pre-change twin on both devices (2.2e-8 on identical frames, which now reach 1).

### Two `float_motion_sycl` divergences found on the way

- `motion_force_zero` was in the option table but only `flush()` read it; `collect()` emitted real motion scores. The CUDA, HIP and Metal twins honour it.
- The debug `VMAF_feature_motion_score` was emitted without `motion_fps_weight`; the CPU emits `motion_clip(score)`. The CUDA twin has the same gap (not changed here).

### Parity per option

CPU vs SYCL twin, per frame, `--precision max`, worst absolute difference over both devices (B580 and UHD 770 agree to the digits shown). Fixtures: the Netflix `src01` 576x324 pair (48 frames, 8-bit 4:2:0); 853x480 4:4:4 8-bit (12 frames) and 10-bit (8 frames), resampled from that pair (the CLI refuses odd widths for 4:2:0); 576x324 4:2:0 10-bit (12 frames, `<< 2` plus LCG dither); an identical 853x480 pair (4 frames). `float_ssim` runs with `scale=1` because 853x480 auto-resolves to 2, which the twin does not implement.

| Twin | Options | nf8 576x324 | 853x480 8-bit | 576x324 10-bit | 853x480 10-bit | identical |
|---|---|---|---|---|---|---|
| `psnr_sycl` | default | 0 | 0 | 0 | 0 | 0 |
| `psnr_sycl` | `enable_mse` | 0 | 0 | 0 | 0 | 0 |
| `psnr_sycl` | `enable_apsnr` | 0 | 0 | 0 | 0 | 0 |
| `psnr_sycl` | `reduced_hbd_peak` | 0 | 0 | 0 | 0 | 0 |
| `psnr_sycl` | `min_sse=0.5` / `min_sse=1e-6` | 0 | 0 | 0 | 0 | 0 |
| `psnr_sycl` | all four + `uncapped` | 0 | 0 | 0 | 0 | 0 |
| `psnr_sycl` | `enable_chroma=false` + `enable_mse` + `enable_apsnr` | 0 | 0 | 0 | 0 | 0 |
| `integer_ssim_sycl` | default / `clip_db` alone | 8.0e-9 | 5.6e-9 | 6.4e-9 | 1.5e-8 | 0 |
| `integer_ssim_sycl` | `enable_db` (± `clip_db`), dB | 3.4e-7 | 1.5e-7 | 1.8e-7 | 5.4e-7 | 0 (`+inf` / ceiling) |
| `float_ssim_sycl` | `scale=1` | 3.1e-7 | 6.4e-7 | 3.3e-8 | 5.1e-8 | 0 |
| `float_ssim_sycl` | `enable_lcs` (worst of l, c, s) | 6.0e-7 | 8.3e-7 | 2.1e-7 | 3.3e-7 | 0 |
| `float_ssim_sycl` | `enable_db` (± `clip_db`, ± `enable_lcs`), dB | 1.6e-5 | 3.4e-5 | 1.9e-6 | 1.6e-6 | 0 (`+inf` / ceiling) |
| `float_motion_sycl` | default / `motion_max_val=10000` | 3.1e-6 | 3.3e-6 | 2.0e-6 | 4.9e-6 | — |
| `float_motion_sycl` | `motion_max_val=4.5` (partly clipped) | 2.8e-6 | 2.1e-6 | 2.0e-6 | 0 | — |
| `float_motion_sycl` | `motion_fps_weight=2:motion_max_val=8` | 5.6e-6 | 0 | 3.9e-6 | 0 | — |
| `float_motion_sycl` | `motion_max_val` 0 / 2 / 2.5 (`mmxv`, `debug=false`) | 0 | 0 | 0 | 0 | — |
| `float_motion_sycl` | `motion_force_zero` | 0 | 0 | 0 | 0 | — |

Every PSNR value, `mse_*` and `apsnr_*` aggregate included, is bit-identical. The dB rows are the linear differences magnified by `4.34 / (1 - ssim)`; the parity-gate tolerance (5e-5) is a linear-domain figure. The baseline-vs-new comparison of the default outputs is bit-identical for `psnr_sycl` and `float_motion_sycl` on both devices.

The CPU `psnr` extractor, now calling `psnr_score.h`, is bit-identical to the previous binary for 8 option sets on the four real fixtures, and the non-slow Netflix golden gate passes on a gcc build (271 passed, 12 skipped).

### The test fails when the implementation is wrong

`test_sycl_twin_option_parity` was run against planted regressions, one at a time, on the B580: the peak ignoring `reduced_hbd_peak` (10-bit `psnr_y` off by 2.6e-2), `mse_*` emitted for luma only, the motion cap removed (`motion2_mmxv_39.9` 51.0 instead of 39.9), the SSIM ratio scaled by `0.9999999` (identical frames 69.2 dB instead of the 93 dB ceiling) and the structure term using the unclamped covariance (`float_ssim_s` off by 1.5e-2). Each failed the test; the unmodified build passes on both devices. Dropping the release of the L/C/S partials fails `test_sycl_init_unwind`.

## Open questions

- The CUDA, HIP and Metal twins still lack these options; `integer_ssim_hip` has no `enable_db` / `clip_db` and `float_ssim_hip` no `enable_lcs`, beyond what the state row listed. They cannot be tested on this host.
- `float_ssim_cuda` declares an `enable_chroma` option the CPU `float_ssim` does not have.

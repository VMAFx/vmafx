<!-- markdownlint-disable MD013 MD060 -->
# ADR-1365: SYCL PSNR, SSIM and float-motion twins take the CPU option tables

- **Status**: Accepted
- **Date**: 2026-09-29
- **Deciders**: lusoris
- **Tags**: sycl, gpu-parity, numerics, feature-extractor, fork-local

## Context

`T-BUG048-GPU-OPTION-PARITY-REMAINDER-2026-09-26` lists CPU options that stale-branch merges removed from the GPU twins. For SYCL: `psnr_sycl` lacked `min_sse`, `enable_mse`, `reduced_hbd_peak` and `enable_apsnr`; `integer_ssim_sycl` and `float_ssim_sycl` lacked `enable_db` / `clip_db`, and `float_ssim_sycl` also `enable_lcs`; `float_motion_sycl` lacked `motion_max_val`. Scores stayed correct because the ADR-1183 gate computes a feature on the CPU when the twin cannot honour a model option, but those features never ran on the device, and naming the twin with the option failed with `unknown option`. RC3 owns the device path ([ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md), [ADR-1352](1352-rc-phase-shift-plus-one.md)).

Constraints: every option must reproduce the CPU semantics exactly; SYCL kernels stay fp64-free ([ADR-0220](0220-sycl-fp64-fallback.md)); no host round trip in the middle of a frame; one behaviour, one implementation (HISS-19). Two facts shaped the design. PSNR, the SSIM dB transform and the motion cap act on per-frame scalars the device already reduces, but `enable_lcs` needs per-pixel luminance, contrast and structure terms. And the CPU contract for a perfect score with `enable_db` is `+inf`, or the `clip_db` ceiling ([ADR-1221](1221-gpu-ms-ssim-db-ceiling.md)): the fp32 SSIM twins did not return exactly 1 for identical windows, so turning on `enable_db` would have reported a finite dB value where the CPU reports the ceiling.

## Decision

We will give the four SYCL twins the CPU option tables (same names, aliases, types, defaults, ranges and flags) and implement every option with the CPU's semantics:

1. **Scalar options on the host, from device-reduced sums.** The PSNR option math (peak, `min_sse` ceiling, MSE to PSNR with `uncapped`, clip-aggregate APSNR) moves verbatim from `integer_psnr.c` into `core/src/feature/psnr_score.h`, which both the CPU extractor and `psnr_sycl` call; `psnr_sycl` gains a `flush` that publishes `apsnr_*`. The SSIM twins convert with the existing `nonfinite_score.h` emitters and a new `vmaf_ssim_max_db()` for the `clip_db` ceiling. `float_motion_sycl` applies the CPU's `motion_clip()` (fps weight, then `motion_max_val`) to every score it emits, the debug `motion` score included, and honours `motion_force_zero`, which it declared but ignored.
2. **`enable_lcs` on the device.** `float_ssim_sycl` selects a second vertical-pass kernel that computes the per-pixel L, C and S terms of `iqa/ssim_tools.c` in fp32 from the same moments and reduces them per work-group next to the SSIM value; the host sums the partials in double. The default kernel is unchanged in structure.
3. **Identical windows score exactly 1.** Both SSIM twins hold every product in a named temporary (icpx does not contract across statements, [ADR-1358](1358-sycl-speed-device-resident-linalg.md)), sum the two variances as a pair, and return 1 when numerator and denominator are equal, so the numerator and denominator run the same operations mirrored. We accept that the default linear scores move by up to 1.1e-8 (2.2e-8 on identical frames, which reach exactly 1).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Host options + device LCS kernel variant + mirrored SSIM arithmetic (chosen) | Every option on the device path; PSNR bit-exact; one PSNR implementation; identical frames follow ADR-1221 | Touches the upstream-mirror `integer_psnr.c`; default SSIM twin outputs move by up to 1.1e-8 | — |
| Keep a SYCL-local copy of the PSNR math (as for `uncapped` before) | No change to the CPU file | Two implementations of the same arithmetic drift; the CUDA / HIP / Metal ports would add three more | HISS-19 |
| Compute L/C/S on the host from read-back moments | No new kernel | Five full-resolution float planes per frame over PCIe, a mid-frame host dependency | Violates device residency; far slower |
| Keep the SSIM formula and document the identical-frame dB gap | Default outputs bit-identical to before | With `enable_db`, identical frames report tens of dB below the CPU's ceiling, or finite instead of `+inf` | Breaks the CPU contract the option exists to reproduce |
| Detect identical frames on the host and substitute 1 | No kernel change | An extra full-frame compare; windows that are identical inside otherwise different frames still miss 1 | Treats the symptom; costs a pass |
| Mark the options `VMAF_OPT_FLAG_DEFAULT_ONLY` ([ADR-1316](1316-gpu-option-value-capability-fallback.md)) | Named requests stop failing | The feature still never runs on the device | Status quo with better errors; RC3 asked for the device path |

## Consequences

- **Positive**: models that set these options keep the four features on the SYCL device. `psnr_sycl` matches the CPU bit for bit for every option; the SSIM and motion options agree within the twins' existing tolerances, and identical frames match the CPU exactly. Two latent `float_motion_sycl` divergences are fixed (`motion_force_zero`, unweighted debug score).
- **Negative**: the default outputs of `integer_ssim_sycl` / `float_ssim_sycl` change by up to 1.1e-8; `integer_psnr.c` now calls a fork header, which an upstream sync has to respect (see the rebase notes). The dB form of SSIM magnifies an fp32 difference by `4.34 / (1 - ssim)`, so near-perfect content can exceed the linear tolerance in dB; this is the twins' fp32 precision, not new error.
- **Neutral / follow-ups**: the CUDA, HIP and Metal twins still lack these options (the row stays open for them); when ported they should call `psnr_score.h` and `vmaf_ssim_max_db()` rather than copy the math. Guarded by `test_sycl_twin_option_parity` and the `enable_lcs` case in `test_sycl_init_unwind`.

## References

- req: task brief for the SYCL part of `T-BUG048-GPU-OPTION-PARITY-REMAINDER-2026-09-26` (2026-09-29): "Implement each option on the device (or in the per-frame host scoring from device-reduced sums — no mid-frame round-trips) with exactly the CPU semantics".
- [Research-2127](../research/2127-sycl-twin-cpu-option-parity.md) — per-option parity on Arc B580 and UHD 770, mutation checks.
- [ADR-1183](1183-model-options-gate-gpu-twin-selection.md) — the option gate; [ADR-1193](1193-psnr-uncapped-option.md) — `uncapped`; [ADR-1221](1221-gpu-ms-ssim-db-ceiling.md) — perfect-score dB contract; [ADR-1302](1302-nonfinite-scores-fail-the-frame.md) — non-finite publication.

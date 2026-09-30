<!-- markdownlint-disable MD013 MD060 -->
# ADR-1382: HIP PSNR, SSIM and float-motion twins take the CPU option tables

- **Status**: Accepted
- **Date**: 2026-09-30
- **Deciders**: lusoris
- **Tags**: hip, gpu-parity, numerics, feature-extractor, fork-local

## Context

`T-BUG048-GPU-OPTION-PARITY-REMAINDER-2026-09-26` lists CPU options the GPU twins lack. [ADR-1365](1365-sycl-twin-cpu-option-parity.md) closed the SYCL part; for HIP: `psnr_hip` lacked `min_sse`, `enable_mse`, `reduced_hbd_peak` and `enable_apsnr`; `integer_ssim_hip` lacked `enable_db` / `clip_db`; `float_ssim_hip` lacked `enable_db`, `clip_db` and `enable_lcs`; `float_motion_hip` lacked `motion_max_val` and emitted its debug `VMAF_feature_motion_score` without `motion_fps_weight` (the CPU emits `motion_clip(score)`). Scores stayed correct because the [ADR-1183](1183-model-options-gate-gpu-twin-selection.md) gate kept any feature with such an option on the CPU, but those features never ran on the device, and naming the twin with the option failed with `unknown option`. The row asks ports to call `psnr_score.h` and `vmaf_ssim_max_db()` instead of copying the math, and to make identical SSIM windows score exactly 1 as ADR-1365 does.

The HIP twins differ from the SYCL ones in two ways that shape the port. HIP kernels may use fp64, and `integer_ssim_hip` already evaluates the CPU's per-pixel expression in double with contraction off, so its per-pixel terms are the CPU's own doubles. `float_ssim_hip` builds with hipcc's default `-ffp-contract=fast`, which fuses products across statements, so named temporaries alone do not keep its numerator and denominator mirrored. HIP-wide fp-contract settings are a separate port (the SYCL counterpart is #1630).

## Decision

We give the four HIP twins the CPU option tables (same names, aliases, types, defaults, ranges and flags) and implement every option with the CPU's semantics:

1. **Scalar options on the host from device-reduced sums.** `psnr_hip` turns each plane's device-reduced integer SSE into `psnr_*`, `mse_*` and the `apsnr_*` aggregates with the helpers of `core/src/feature/psnr_score.h` that the CPU extractor calls (`vmaf_psnr_peak()`, `vmaf_psnr_max()`, `vmaf_psnr_from_mse()`, `vmaf_psnr_aggregate()`), accumulating the APSNR totals in `collect()` and publishing them in a new `flush()`. `integer_ssim_hip` / `float_ssim_hip` pass `enable_db` and `vmaf_ssim_max_db()` to the shared `nonfinite_score.h` emitters. `float_motion_hip` sends every score it emits, the debug `motion` score and the flush tail included, through the CPU's `motion_clip()` (fps weight, then `motion_max_val`).
2. **`enable_lcs` on the device.** `float_ssim_hip` selects `calculate_ssim_hip_vert_combine_lcs`, a pass-2 variant that also computes the per-pixel L, C and S of `iqa/ssim_tools.c` in the CPU's own types (fp32 inputs and clamped variances, double L and C, fp32 S with the flat-window covariance clamp) and reduces them per block in double; the host sums the blocks and emits `float_ssim_{l,c,s}` with the score in CPU order.
3. **Identical frames follow the CPU.** `integer_ssim_hip` keeps the CPU's double expression operand for operand and returns the window weight when numerator and denominator factors are equal (always, for 8- and 10-bit input): the CPU's raster-order sum absorbs the ulp its quotient leaves, the twin's per-block tree does not, and with the rule identical frames from 3x3 up score exactly 1 on both (a host replay of the CPU `calc_ssim()`; 1x1 and 2x2 remain, `T-HIP-INTEGER-SSIM-TINY-IDENTICAL-DB-2026-09-30`). `float_ssim_hip` does not force anything: its pass 2 forms each pixel's term as the CPU's `ssim_accumulate_default_scalar()` does, `l * c * s` in double from the CPU-typed factors (`ssim_lcs()`, contraction off), sums one double per block, and the host rounds the frame mean to fp32 as `iqa_ssim()` returns it. The CPU itself is not exactly 1 on every identical frame (72.247 dB = 1 - 2^-24 on a flat 64x64 frame, from its fp32 luminance denominator), so a forced 1 would disagree with it; the reproduced arithmetic agrees wherever the vertical moments agree.
4. **The rest of the CPU contract of these twins, found in review.** `motion_v2_hip` stores its SAD as the CPU does, `MIN(score * motion_fps_weight, motion_max_val)`, and folds `motion2_v2` / `motion3_v2` from the stored value; a one-frame run emits both as 0. `psnr_hip` carries `VMAF_FEATURE_EXTRACTOR_TEMPORAL` like the CPU `psnr`, so `--subsample N > 1` does not drop frames from the `apsnr_*` totals. `motion_hip` defaults `debug` to false and emits `VMAF_integer_feature_motion_sad_score` every frame, the CPU `motion`'s set. `scripts/ci/cross_backend_parity_gate.py` gains the `hip` backend and a `float_ssim_lcs` cell (`float_ssim` with `enable_lcs=true`, 5e-5), and `test_hip_twin_option_parity` holds `float_ssim` and its L / C / S to that gate tolerance.
5. **One HIP error translator.** The four twins drop their private copies of the `hipError_t` to errno mapping and call `vmaf_hip_rc_to_errno()` (`core/src/hip/common.h`), which `kernel_template.c` now defines; the mappings were identical (HISS-19).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Host options + device LCS variant + CPU per-pixel float SSIM + exact-1 integer windows (chosen) | Every option on the device path; PSNR bit-exact through the CPU's own helpers; both SSIM twins compute each pixel's term the CPU's way; identical frames agree with the CPU | New flush for `psnr_hip`; `float_ssim_hip` pass 2 works in double (as its `enable_lcs` variant already did) | — |
| Force an identical `float_ssim` window to exactly 1 (ADR-1365's SYCL form) | Identical frames always `+inf` | The CPU reports 72.247 dB on a flat identical frame, so the twin disagrees with its reference exactly where `enable_db` magnifies the difference; keeps the combined-formula residual (`T-SYCL-FLOAT-SSIM-COMBINED-FORMULA-RESIDUAL-2026-09-29`) | The CPU is the reference; HIP has correctly rounded `sqrtf` and division, so the product form needs no equal-operand guard |
| Port ADR-1365's fp32 SSIM form to `integer_ssim_hip` too | Same code shape as SYCL | Throws away a double per-pixel term that is already the CPU's exactly | The HIP kernel is closer to the CPU as it is |
| Build `ssim_score.hip` with `-ffp-contract=off` | No pragma | Changes the moment passes too; overlaps the pending fp-contract port | Out of scope here; the pragma is local to the per-pixel formula |
| Compute L/C/S on the host from read-back moments | No new kernel | Five full-resolution planes over PCIe per frame, a host round trip mid-frame | Violates device residency (maintainer: no GPU-CPU round trips) |
| Keep a HIP-local copy of the PSNR math | No include of a CPU header | Duplicate arithmetic drifts; the row asks for `psnr_score.h` | HISS-19 |
| Mark the options `VMAF_OPT_FLAG_DEFAULT_ONLY` ([ADR-1316](1316-gpu-option-value-capability-fallback.md)) | Named requests stop failing | The feature still never runs on the device | Status quo with better errors |

## Consequences

- **Positive**: models that set these options keep `psnr`, `ssim`, `float_ssim` and `float_motion` on the HIP device; `psnr_hip` is expected to match the CPU bit for bit with every option (integer SSE, same host helpers), the SSIM and motion options within the twins' existing tolerances, and identical frames exactly. `float_motion_hip`'s debug score now carries the fps weight like the CPU.
- **Negative**: not measured on AMD hardware in this change; `test_hip_twin_option_parity` carries the per-option device checks and skips without a device. `float_ssim_hip`'s default score moves from the combined Wang formula to the CPU's product form (the SYCL measurements put the formula residual at up to 7.8e-5 and the product form within 9e-7 of the CPU); bit-exact identical-frame dB also needs the vertical moments to match the CPU's, which the pending fp-contract work decides. `float_motion_hip` still lacks `motion3` and five options (`T-HIP-FLOAT-MOTION-MOTION3-OPTIONS-2026-09-30`).
- **Neutral / follow-ups**: the CUDA and Metal parts of the row stay open. Guarded by `test_hip_twin_option_parity` (the option-table and unknown-option checks need no device), `test_gpu_psnr_option_parity_contract.py`, `test_gpu_option_alias_contract.py` (`float_motion_hip` `motion_max_val` alias `mmxv`), and the option cases of `test_hip_kernel_source_contract.py`.

## References

- req: RC3 port brief (2026-09-30): "psnr options via core/src/feature/psnr_score.h; integer_ssim_hip enable_db/clip_db; float_ssim_hip enable_lcs; float_motion motion_max_val; debug motion fps weight", and "there shouldnt be any gpu cpu rountrips".
- [ADR-1365](1365-sycl-twin-cpu-option-parity.md) and [Research-2127](../research/2127-sycl-twin-cpu-option-parity.md) — the SYCL port this mirrors.
- [Research-1377](../research/1377-hip-rc3-cpu-parity.md) — why the integer SSIM term needs the identical-window rule.
- [ADR-1183](1183-model-options-gate-gpu-twin-selection.md), [ADR-1193](1193-psnr-uncapped-option.md), [ADR-1221](1221-gpu-ms-ssim-db-ceiling.md), [ADR-1302](1302-nonfinite-scores-fail-the-frame.md), [ADR-0564](0564-integer-ssim-gpu-real-kernels.md).

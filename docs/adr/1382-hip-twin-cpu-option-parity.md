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
3. **Identical windows score exactly 1.** `integer_ssim_hip` keeps the CPU's double expression operand for operand and returns the window weight when numerator and denominator factors are equal (always, for 8- and 10-bit input). `float_ssim_hip` computes its per-pixel value in `ssim_from_moments()` with `#pragma clang fp contract(off)` and named products, mirrored like ADR-1365, and returns 1 when numerator and denominator are equal. Identical frames then report the CPU's `+inf` / `clip_db` ceiling.
4. **One HIP error translator.** The four twins drop their private copies of the `hipError_t` to errno mapping and call `vmaf_hip_rc_to_errno()` (`core/src/hip/common.h`), which `kernel_template.c` now defines; the mappings were identical (HISS-19).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Host options + device LCS variant + exact-1 identical windows (chosen) | Every option on the device path; PSNR bit-exact through the CPU's own helpers; integer SSIM keeps the CPU's per-pixel doubles; identical frames follow ADR-1221 | New flush for `psnr_hip`; `float_ssim_hip`'s default linear score moves at fp32 rounding level because its per-pixel products are no longer fused | — |
| Port ADR-1365's fp32 SSIM form to `integer_ssim_hip` too | Same code shape as SYCL | Throws away a double per-pixel term that is already the CPU's exactly | The HIP kernel is closer to the CPU as it is |
| Build `ssim_score.hip` with `-ffp-contract=off` | No pragma | Changes the moment passes too; overlaps the pending fp-contract port | Out of scope here; the pragma is local to the per-pixel formula |
| Compute L/C/S on the host from read-back moments | No new kernel | Five full-resolution planes over PCIe per frame, a host round trip mid-frame | Violates device residency (maintainer: no GPU-CPU round trips) |
| Keep a HIP-local copy of the PSNR math | No include of a CPU header | Duplicate arithmetic drifts; the row asks for `psnr_score.h` | HISS-19 |
| Mark the options `VMAF_OPT_FLAG_DEFAULT_ONLY` ([ADR-1316](1316-gpu-option-value-capability-fallback.md)) | Named requests stop failing | The feature still never runs on the device | Status quo with better errors |

## Consequences

- **Positive**: models that set these options keep `psnr`, `ssim`, `float_ssim` and `float_motion` on the HIP device; `psnr_hip` is expected to match the CPU bit for bit with every option (integer SSE, same host helpers), the SSIM and motion options within the twins' existing tolerances, and identical frames exactly. `float_motion_hip`'s debug score now carries the fps weight like the CPU.
- **Negative**: not measured on AMD hardware in this change; `test_hip_twin_option_parity` carries the per-option device checks and skips without a device. `float_ssim_hip`'s default score can move at the fp32 rounding level (the SYCL port measured 1.1e-8).
- **Neutral / follow-ups**: the CUDA and Metal parts of the row stay open. Guarded by `test_hip_twin_option_parity` (the option-table and unknown-option checks need no device), `test_gpu_psnr_option_parity_contract.py`, `test_gpu_option_alias_contract.py` (`float_motion_hip` `motion_max_val` alias `mmxv`), and the option cases of `test_hip_kernel_source_contract.py`.

## References

- req: RC3 port brief (2026-09-30): "psnr options via core/src/feature/psnr_score.h; integer_ssim_hip enable_db/clip_db; float_ssim_hip enable_lcs; float_motion motion_max_val; debug motion fps weight", and "there shouldnt be any gpu cpu rountrips".
- [ADR-1365](1365-sycl-twin-cpu-option-parity.md) and [Research-2127](../research/2127-sycl-twin-cpu-option-parity.md) — the SYCL port this mirrors.
- [Research-1377](../research/1377-hip-rc3-cpu-parity.md) — why the integer SSIM term needs the identical-window rule.
- [ADR-1183](1183-model-options-gate-gpu-twin-selection.md), [ADR-1193](1193-psnr-uncapped-option.md), [ADR-1221](1221-gpu-ms-ssim-db-ceiling.md), [ADR-1302](1302-nonfinite-scores-fail-the-frame.md), [ADR-0564](0564-integer-ssim-gpu-real-kernels.md).

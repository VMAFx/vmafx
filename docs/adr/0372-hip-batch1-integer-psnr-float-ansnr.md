<!-- markdownlint-disable MD013 MD060 -->
# ADR-0372: HIP Batch-1 — `integer_psnr_hip` and `float_ansnr_hip` Real Kernels

- **Status**: Accepted
- **Date**: 2026-05-10
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `hip`, `gpu`, `build`

## Context

After the T7-10b HIP runtime PR landed (2026-05-08), the kernel-template helpers
(`hipStreamCreateWithFlags`, `hipMalloc`, `hipMemcpyAsync`, etc.) are real. All
eight HIP feature-extractor stubs registered correctly but returned `-ENOSYS` from
`submit()`/`collect()` because no device-side kernels existed and the module-load
path was absent from the host `.c` files.

PR #612 (`float_psnr_hip`, ADR-0254) established the canonical HIP module-API
pattern: `hipcc --genco` produces a HSACO fat binary; `xxd -i` embeds it; the
host calls `hipModuleLoadData` + `hipModuleGetFunction` at `init()` time and
`hipModuleLaunchKernel` at `submit()` time. This ADR records the decision to apply
the same pattern to `integer_psnr_hip` and `float_ansnr_hip` as batch-1 (the two
simplest scalar-reduction targets).

The four batch-1 candidate targets were: `float_ssim_hip`, `float_ansnr_hip`,
`integer_psnr_hip`, and `motion_hip`. `float_ssim_hip` was deferred because its
two-pass design (horizontal + vertical Gaussian passes, five intermediate device
buffers, three kernel functions) required non-trivial ABI adaptation beyond a
mechanical port. `motion_hip` was deferred because the existing stub used an
entirely different API shape (raw `vmaf_hip_motion_*` functions, not a
`VmafFeatureExtractor`) and the CUDA twin carried complex temporal state. The two
chosen targets (`integer_psnr_hip` and `float_ansnr_hip`) are single-dispatch,
plain-pointer-interface kernels whose numerical equivalence with the CPU reference
is straightforward to verify.

## Decision

Promote `integer_psnr_hip` (emits `psnr_y`, luma-only v1) and `float_ansnr_hip`
(emits `float_ansnr` + `float_anpsnr`) from `-ENOSYS` stubs to real HIP module-API
consumers following the pattern established by PR #612 (ADR-0254). Device kernels
live in `hip/integer_psnr/psnr_score.hip` and `hip/float_ansnr/float_ansnr_score.hip`.
Both use GCN/RDNA warp-size-64 reductions (`__shfl_down` without mask). The meson
`enable_hipcc` option (added by ADR-0254 / PR #612; already in `meson_options.txt`)
controls compilation; without it, `init()` returns `-ENOSYS` (scaffold posture
preserved for non-ROCm builds). The new `vmaf_hip_kernel_submit_post_record` helper
is added to `kernel_template.{h,c}` (same helper PR #612 adds; on merge conflict,
keep one copy).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Port all four targets (incl. float_ssim + motion) | Bigger batch | float_ssim needs non-trivial buffer-alloc adaptation; motion needs complete API rewrite | Risk of subtle numeric divergence; per-task instruction says STOP if equivalence is non-trivial |
| Use hipLaunchKernelGGL instead of Module API | Simpler host code | Requires C++ host TUs; existing files are plain C | PR #612 / ADR-0254 established Module API as canonical; consistency wins |
| Port only one kernel | Less risk | Misses batch economy | Both integer_psnr and float_ansnr are mechanical; doing both in one PR is net positive |

## Consequences

- **Positive**: HIP extractor count increases from 1/11 real (after PR #612) to
  3/11 real (or 2/11 if PR #612 is still in flight at merge time). Proves the
  pattern works for both uint64-atomic-SSE and per-block-float-partials styles.
- **Negative**: Merge conflict with PR #612 is possible on `kernel_template.{h,c}`
  (the `submit_post_record` helper). Resolution: keep the existing copy, discard
  the duplicate.
- **Neutral / follow-ups**: `float_ssim_hip` and `motion_hip` remain at `-ENOSYS`;
  they are candidates for batch-2. Chroma extension for `integer_psnr_hip` (to emit
  `psnr_cb`/`psnr_cr`) is a follow-up matching the CUDA twin's T3-15(a) extension.

## References

- ADR-0241 (`integer_psnr_hip` first consumer / kernel-template scaffold).
- ADR-0254 (`float_psnr_hip` first real kernel / canonical HIP module-API pattern).
- ADR-0266 (`float_ansnr_hip` fifth consumer scaffold).
- PR #612 (`float_psnr_hip` first real kernel, in flight at time of this PR).
- paraphrased: the user requested a new PR implementing 3-4 of the 11 HIP `-ENOSYS` stubs with real kernels, targeting the simplest scalar reductions first and following the pattern proven by PR #612.

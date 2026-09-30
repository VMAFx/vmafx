<!-- markdownlint-disable MD013 MD060 -->
# ADR-1373: CUDA PSNR, SSIM and float-motion twins take the CPU option tables

- **Status**: Accepted
- **Date**: 2026-09-30
- **Deciders**: lusoris
- **Tags**: cuda, gpu-parity, numerics, feature-extractor, fork-local

## Context

`T-BUG048-GPU-OPTION-PARITY-REMAINDER-2026-09-26` lists CPU options that stale-branch merges removed from the GPU twins. [ADR-1365](1365-sycl-twin-cpu-option-parity.md) closed the SYCL part. For CUDA: `psnr_cuda` lacked `min_sse`, `enable_mse`, `reduced_hbd_peak` and `enable_apsnr`; `integer_ssim_cuda` lacked `enable_db` / `clip_db`; `float_ssim_cuda` lacked `enable_lcs`, `enable_db` and `clip_db` and declared an `enable_chroma` the CPU `float_ssim` does not have (its kernel read luma only, so the option did nothing); `float_motion_cuda` lacked `motion_max_val` and emitted its debug `motion` score without `motion_fps_weight`. Scores stayed correct, because the [ADR-1183](1183-model-options-gate-gpu-twin-selection.md) gate keeps a feature on the CPU when the twin cannot honour a model option, but those features never ran on the device, and naming the twin with the option failed with `unknown option`.

Constraints: every option must reproduce the CPU semantics exactly; no host round trip in the middle of a frame and one wait per frame, at collect (req); one behaviour, one implementation (HISS-19). The CPU contract for a perfect SSIM with `enable_db` is `+inf`, or the `clip_db` ceiling ([ADR-1221](1221-gpu-ms-ssim-db-ceiling.md)). `float_ssim_cuda` compiles with NVCC's default FMA contraction, which rounds the numerator and the denominator of the SSIM combine differently, so identical windows did not score exactly 1.

## Decision

We will give the four CUDA twins their CPU extractor's option tables (same names, aliases, types, defaults, ranges and flags) and implement every option with the CPU's semantics, following ADR-1365:

1. **Scalar options on the host, from device-reduced sums, through the CPU's helpers.** `psnr_cuda` turns each plane's device SSE into `psnr_*`, `mse_*` and (in a new `flush`) `apsnr_*` with `core/src/feature/psnr_score.h`, which the CPU `integer_psnr.c` calls too; its local copy of the MSE-to-PSNR arithmetic is deleted. The SSIM twins apply `enable_db` / `clip_db` through the `nonfinite_score.h` emitters with `vmaf_ssim_max_db()`. `float_motion_cuda` applies the CPU's `motion_clip()` (fps weight, then `motion_max_val`) to every score it emits, the debug `motion` score included.
2. **`enable_lcs` on the device.** `float_ssim_cuda` launches a second pass-2 kernel that computes the per-pixel L, C and S terms of `iqa/ssim_tools.c` in fp32 from the same moments and reduces them per block next to the SSIM value; the host sums the partials in double. The default kernel keeps its structure.
3. **Identical windows score exactly 1.** The `float_ssim_cuda` combine rounds its three products with `__fmul_rn`, which NVCC never contracts, so the numerator and the denominator run the same operations mirrored and compare equal for identical windows; the kernel then returns exactly 1. `integer_ssim_cuda` already evaluates the CPU's per-pixel double expression (`integer_ssim.c::ssim_reduce_row_range`); but NVCC's default contraction did not compile it as written: the master PTX rounds the stability constant `c1 = sm^2 * K1 * w * w` for the numerator and fuses its last multiply into the denominator's sum, and fuses `c2` and `y2 * w` the same way. The kernel now builds with `--fmad=false`, as the HIP twin builds with `-ffp-contract=off` ([ADR-0564](0564-integer-ssim-gpu-real-kernels.md)), so the expression is the CPU's operand for operand. At 8 and 10 bits the moments and their products are integers below 2^53, so an identical window then gives numerator == denominator by construction and scores exactly 1.
4. **`enable_chroma` leaves `float_ssim_cuda`.** The CPU extractor has no such option; the twin declared it but read luma only.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Host options from device sums through the CPU helpers + device L/C/S kernel + uncontracted SSIM products (chosen) | Every option on the device path; PSNR bit-exact; one PSNR implementation for CPU, SYCL and CUDA; identical frames follow ADR-1221 | Default `float_ssim_cuda` output moves by an fp32 rounding (the products are no longer fused) | — |
| Keep a CUDA-local copy of the PSNR math | No shared-header dependency | Two implementations of the same arithmetic drift apart | HISS-19 |
| Compute L/C/S on the host from read-back moments | No new kernel | Five full-resolution float planes over PCIe per frame, a mid-frame host dependency | Violates the no-round-trip requirement |
| Compile the float SSIM kernel with `--fmad=false` | No source change to the combine | Also unfuses every tap of the two 11-tap passes, moving the default score more and costing throughput | `__fmul_rn` confines the change to the three products that must mirror |
| Leave `integer_ssim_score` with FMA contraction | No build change; identical noise frames still emulate to exactly 1 | The per-pixel term is not the CPU's expression at any bit depth, and identical windows compare a rounded `c1` against an unrounded one, equal only by luck of the rounding | The HIP twin already sets the equivalent flag; the combine is one expression per pixel, so the cost is nil |
| Keep `enable_chroma` as a no-op for compatibility | No option-table change | A twin-only key the CPU rejects; a model cannot use it, and naming it on the twin claims chroma support the kernel lacks | The CPU table is the authority (ADR-1183) |
| Mark the options `VMAF_OPT_FLAG_DEFAULT_ONLY` ([ADR-1316](1316-gpu-option-value-capability-fallback.md)) | Named requests stop failing | The features still never run on the device | Status quo with better errors |

## Consequences

- **Positive**: models that set these options keep the four features on the CUDA device. `psnr_cuda` matches the CPU bit for bit for every option (integer SSE, the same host arithmetic); the SSIM and motion options agree within the twins' existing tolerances, and identical frames report the CPU's `+inf` / `clip_db` ceiling. `float_motion_cuda`'s debug `motion` score carries `motion_fps_weight` like the CPU's.
- **Negative**: `--feature float_ssim_cuda=enable_chroma=...` now fails with `unknown option`; the option never changed a score. Default `float_ssim_cuda` output moves by an fp32 rounding (ADR-1365 measured at most 1.1e-8 for the same change on SYCL); default `integer_ssim_cuda` per-pixel terms move by a double rounding (about 1e-16 relative) towards the CPU's. Not measured on NVIDIA hardware here.
- **Neutral / follow-ups**: the HIP and Metal twins still lack these options (the row stays open for them; `float_ms_ssim_cuda` `enable_chroma` stays with `T-MS-SSIM-GPU-CHROMA-OPTION-DRIFT-2026-09-06`). Guarded by `test_cuda_twin_option_parity` (option tables device-free; CPU parity with a device) and the option cases of `test_cuda_kernel_source_contract.py`.

## References

- req: "there shouldnt be any gpu cpu rountrips" (user, relayed in the RC3 CUDA port brief, 2026-09-30).
- req: RC3 CUDA port brief (2026-09-30): "psnr enable_mse/enable_apsnr/reduced_hbd_peak/min_sse through the shared core/src/feature/psnr_score.h; integer ssim enable_db/clip_db; float_ssim enable_lcs/enable_db/clip_db and drop the phantom enable_chroma; float_motion motion_max_val; the debug motion score missing motion_fps_weight".
- [Research-1372](../research/1372-cuda-rc3-parity-port.md).
- [ADR-1365](1365-sycl-twin-cpu-option-parity.md), [ADR-1183](1183-model-options-gate-gpu-twin-selection.md), [ADR-1221](1221-gpu-ms-ssim-db-ceiling.md), [ADR-1302](1302-nonfinite-scores-fail-the-frame.md), [ADR-1193](1193-psnr-uncapped-option.md).

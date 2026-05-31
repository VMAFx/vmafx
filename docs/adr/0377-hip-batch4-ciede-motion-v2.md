<!-- markdownlint-disable MD060 -->
# ADR-0377: HIP batch-4 — `ciede_hip` and `integer_motion_v2_hip` real kernels

- **Status**: Accepted
- **Date**: 2026-05-10
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `hip`, `gpu`, `build`, `feature-extractor`, `fork-local`

## Context

After batch-1 (ADR-0372: `integer_psnr_hip`, `float_ansnr_hip`), batch-2
(ADR-0373: `float_motion_hip`), and batch-3 (ADR-0375: `float_ssim_hip`,
`float_moment_hip`), four HIP extractors remain at `-ENOSYS` scaffold
posture: `ciede_hip`, `integer_motion_v2_hip`, `adm_hip`, and `vif_hip`.

`ciede_hip` (ADR-0259) and `integer_motion_v2_hip` (ADR-0267) already
have the correct `VmafFeatureExtractor` lifecycle shape with
`init/submit/collect/close/flush` callbacks and `#ifdef HAVE_HIPCC`
scaffolding — they require only a real HIP module load + kernel dispatch
to be promoted. Their CUDA twins are straightforward to port.

`adm_hip` and `vif_hip` use a different low-level API shape
(`vmaf_hip_adm_init / vmaf_hip_adm_run / vmaf_hip_adm_destroy`) inherited
from the initial scaffold (ADR-0212), not the `VmafFeatureExtractor`
lifecycle pattern used by all promoted extractors. `adm_hip` also requires
six separate CUDA kernels (DWT2, CSF, CSF-den, decouple, CM). Promoting
either would require a full host TU redesign, not an incremental fill-in,
and the multi-kernel ADM pipeline carries non-trivial equivalence risk.
They are deferred to a separate effort.

## Decision

Promote `ciede_hip` and `integer_motion_v2_hip` to real HIP module-API
consumers in this batch. Ship `adm_hip` and `vif_hip` as STOPs with a
clear redesign note in the overview doc.

### `ciede_hip`

Device kernel `hip/integer_ciede/ciede_score.hip`: direct port of the CUDA
twin (`cuda/integer_ciede/ciede_score.cu`). Per-pixel YUV-to-Lab conversion
(BT.709 primaries, same constants as CUDA twin), CIEDE2000 ΔE computation,
per-warp reduction using `__shfl_down` (warp-64 on GCN/RDNA, no mask
argument), per-block float partial written to a flat `float *partials`
buffer. Host accumulates partials in `double`, applies
`45 - 20 * log10(mean_dE)`. Kernel signature differs from CUDA: takes
six raw pointer + stride arguments (ref Y/U/V + dis Y/U/V) instead of
two `VmafPicture` structs, since HIP pictures arrive as CPU-side
`VmafPicture` and require explicit HtoD copies.

Host TU: six staging `hipMalloc` buffers (ref_y, ref_u, ref_v, dis_y,
dis_u, dis_v). Submit performs six `hipMemcpy2DAsync` calls (HtoD), then
launches the kernel, records submit event, enqueues DtoH partial copy,
and calls `vmaf_hip_kernel_submit_post_record`. Helpers
`ciede_hip_bufs_alloc` / `ciede_hip_bufs_free` extracted to keep each
function under 60 lines (readability-function-size limit).

### `integer_motion_v2_hip`

Device kernel `hip/integer_motion_v2/motion_v2_score.hip`: direct port of
the CUDA twin (`cuda/integer_motion_v2/motion_v2_score.cu`). Shared-memory
tile for diff(prev, cur), 5-tap separable Gaussian filter in X then Y,
per-pixel `|blurred|` accumulated into a single uint64 via `atomicAdd`.
Mirror padding uses reflective mirror (`2 * size - idx - 1`) — differs
from `motion_hip`'s skip-boundary mirror, matching the CPU and CUDA
motion_v2 references.

**Bit-exactness (ADR-0138/0139):** the inner right-shifts use signed
arithmetic (`int32_t` and `int64_t` types with C++ arithmetic shift
semantics), matching the CPU reference exactly. A logical (unsigned) shift
on the int64 intermediate would produce wrong results for negative signed
values — this was the root cause of the AVX2 `srlv_epi64` regression
fixed in PR #587. The HIP kernel uses `__shfl_down` for warp-64 reduction
(GCN/RDNA), no mask argument.

Host TU: two `hipMalloc` ping-pong buffers (`pix[0]`, `pix[1]`). On each
submit the current ref Y plane is HtoD-copied into `pix[index % 2]`. Frame
0 records only the submit event (no kernel, collect emits 0). Subsequent
frames memset the uint64 SAD accumulator to 0, launch the kernel, DtoH
copy the SAD value, then call `vmaf_hip_kernel_submit_post_record`. Host
`flush()` post-pass computes `motion2_v2 = min(score[i], score[i+1])`
exactly as the CUDA twin.

### Meson

`ciede_score` and `motion_v2_score` added to `hip_kernel_sources` in
`core/src/meson.build` (batch-4 comment added to the existing
batch-1/2/3 comment block). HIP real-kernel count: 8/11.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Also promote `adm_hip` | Reaches 7/11 | Requires full VmafFeatureExtractor redesign of the host TU + 6 CUDA kernels; multi-stage DWT pipeline with non-trivial equivalence risk | Not a clean fill-in; separate effort needed |
| Also promote `vif_hip` | Reaches 8/11 | Same wrong API shape as `adm_hip`; multi-scale VIF dispatch is complex | Same as above |
| Single atomic float instead of per-block partials for ciede | Simpler kernel | Empirically off by ~2 in the score for 1080p (ADR-0187) | Already ruled out by CUDA twin; double accumulation retained |

## Consequences

- **Positive**: HIP real-kernel count advances from 6/11 to 8/11.
  `ciede_hip` and `motion_v2_hip` are now usable on ROCm hardware.
  `adm_hip` / `vif_hip` redesign can be a focused follow-up without
  blocking this batch.
- **Negative**: `adm_hip` and `vif_hip` remain at `-ENOSYS`; the HIP
  backend is still not fully complete (8/11).
- **Neutral**: batch-3 (ADR-0375: `float_moment_hip`, `float_ssim_hip`)
  merged prior to this batch-4 PR.

## References

- ADR-0259: `ciede_hip` scaffold (third kernel-template consumer).
- ADR-0267: `integer_motion_v2_hip` scaffold (sixth kernel-template consumer).
- ADR-0372: HIP batch-1 (`integer_psnr_hip`, `float_ansnr_hip`).
- ADR-0373: HIP batch-2 (`float_motion_hip`).
- ADR-0138 / ADR-0139: bit-exactness posture (arithmetic vs logical shift).
- ADR-0187: ciede precision argument (double accumulation, places=4).
- PR #587: AVX2 `srlv_epi64` logical-shift regression fix (motion_v2).

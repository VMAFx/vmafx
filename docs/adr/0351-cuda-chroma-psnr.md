<!-- markdownlint-disable MD060 -->
# ADR-0351: CUDA PSNR — chroma extension (psnr_cb / psnr_cr)

- **Status**: Accepted
- **Date**: 2026-05-09
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: `cuda`, `gpu`, `feature-extractor`, `psnr`

## Context

`psnr_cuda` (T7-23 / [ADR-0182](0182-gpu-long-tail-batch-1.md))
landed luma-only — `provided_features = {"psnr_y", NULL}`, a single
dispatch per frame against `data[0]` / `stride[0]`, and a single
device SSE accumulator. The original header comment justified the
omission with "the picture_cuda upload path is luma-only today",
but on inspection that turned out to be stale: since
[ADR-0182](0182-gpu-long-tail-batch-1.md)'s batch 1c
(`integer_ciede_cuda`, the first chroma-aware CUDA extractor)
landed, `libvmaf.c::translate_picture_host` has uploaded all three
planes for any non-`YUV400P` input — see the `upload_mask` branch
in `core/src/libvmaf.c`. The luma-only kernels (psnr / motion /
adm / vif / moment) just don't read `data[1..2]`.

CPU `integer_psnr.c` emits `psnr_y / psnr_cb / psnr_cr`
unconditionally on YUV420/422/444 (clamping to luma-only on YUV400
via `enable_chroma = false`). Until this ADR, any pipeline asking
the CUDA backend for chroma PSNR fell through to CPU because the
extractor's `provided_features` claimed only `psnr_y`. Closing that
gap is part of the GPU long-tail backlog (T3-15 part b) and pairs
with [ADR-0216](0216-vulkan-chroma-psnr.md), which closed the same
gap on the Vulkan twin.

## Decision

Extend `psnr_cuda.c` to dispatch the existing `psnr_score.cu`
kernel up to three times per frame — once each for Y, Cb, Cr —
against per-plane data pointers and per-plane `(width, height)`
launch arguments. The kernel is generalised by adding a `plane`
parameter so it indexes `ref.data[plane] / ref.stride[plane]`
instead of the hard-coded `[0]`. State carries `rb[3]` (one
device-SSE-accumulator + pinned-host-readback pair per plane);
the per-frame async lifecycle (private stream + submit/finished
event pair) stays singleton — the picture stream issues all
per-plane launches back-to-back with no inter-plane barrier (the
accumulators are independent), and the readback stream serially
DtoHs all three slots before recording `finished`. The host
fence-waits once and reduces all three SSE values serially.
`provided_features` becomes `{"psnr_y", "psnr_cb", "psnr_cr"}`.
YUV400P clamps `n_planes = 1` so chroma dispatches and emits are
skipped at runtime.

`psnr_max[p]` follows the CPU `integer_psnr.c` default branch
(`min_sse == 0`): `psnr_max[p] = (6 * bpc) + 12`, identical for
all three planes. The `min_sse`-driven per-plane formula is left
unimplemented (no shipped extractor sets `min_sse` on the CUDA
path); the array layout makes it a one-line change if needed.
Mirrors the same posture chosen for Vulkan in ADR-0216.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Three-element arrays + plane kernel arg (chosen)** | Minimal diff against the v1 luma-only state; one PTX module + one pair of kernel functions covers all planes; aligns with the Vulkan chroma extension's array shape; close to the canonical CPU `integer_psnr.c` layout for the rebase story. | Per-plane arrays read slightly more verbosely than a singleton; three sequential DtoH copies on `lc.str` (still one host wait point). | Smallest, most reviewable shape; matches the CPU and Vulkan twins. |
| Three independent `PsnrStateCuda` instances dispatched as a meta-extractor | Clean per-plane isolation. | Triples stream + event allocations, accumulator buffers, pinned host slots, and `feature_name_dict` instances; CPU `integer_psnr.c` and Vulkan `psnr_vulkan.c` both use the array shape — staying close to the canonical layout helps the rebase story. | Wrong cost / benefit. |
| Per-plane CUfunction handles (e.g. `calculate_psnr_kernel_y_8bpc / _cb_8bpc / _cr_8bpc`) via specialised entry-points | Lets the kernel make plane-specific compile-time decisions. | The kernel already takes `(width, height)` as launch args — there is *no* plane-specific decision the .cu code needs to make. Six function handles for zero benefit. | Function-specialisation buys nothing. |
| Single dispatch covering all planes via branched indexing inside the kernel | Would let one launch handle all three planes. | Defeats the per-warp uint64 atomic reduction pattern (every warp would need to know which plane it owned and which accumulator to atomicAdd into); kills bit-exactness against CPU; needs three independent shared-memory accumulators per WG. | Way more complex than three small dispatches. |

## Consequences

- **Positive**:
  - CUDA PSNR now matches the CPU `provided_features` set —
    pipelines that name `psnr_cb` / `psnr_cr` are routed to CUDA
    instead of silently falling through to CPU.
  - Cross-backend gate
    (`scripts/ci/cross_backend_vif_diff.py --feature psnr
    --backend cuda`) covers all three plane scores at
    `places=4`; measured `max_abs_diff = 0.0` across 48 frames at
    576 x 324 *and* 640 x 480 (RTX 4090, 8-bit 4:2:0). Both sides
    use deterministic int64 SSE accumulators on integer YUV
    inputs.
  - Pairs with [ADR-0216](0216-vulkan-chroma-psnr.md) (Vulkan
    chroma PSNR) — the GPU long-tail backlog row "psnr chroma
    parity with CPU" is now closed across both shipping GPU
    backends.
  - Unblocks the chroma SSIM / chroma MS-SSIM CUDA follow-ups
    (T-rows queued separately), which need the same per-plane
    upload / dispatch / readback pattern.
- **Negative**:
  - Up to 3 dispatches per frame instead of 1 (`.chars
    .n_dispatches_per_frame` bumped 1 → 3). Per-plane grids are
    smaller, so wall-time impact is sub-linear — chroma
    dispatches at 4:2:0 cover ~25 % of luma area each. Three
    sequential DtoH copies on the readback stream add ~24 B of
    transfer per frame.
  - `PsnrStateCuda` size grows by `2 * sizeof(VmafCudaKernelReadback)`
    (~48 B) plus the per-plane geometry arrays. Negligible.
- **Neutral / follow-ups**:
  - Chroma SSIM (`integer_ssim_cuda` chroma extension) — separate
    row.
  - Chroma MS-SSIM (`integer_ms_ssim_cuda` chroma extension) —
    separate row, gated on chroma SSIM landing.
  - The `min_sse`-driven `psnr_max[p]` branch from CPU
    `integer_psnr.c` is intentionally not replicated; reactivate
    when a shipped extractor configuration sets `min_sse`.
  - HIP twin (`integer_psnr_hip`, currently in scaffold-only
    posture per [ADR-0241](0241-hip-first-consumer-psnr.md))
    inherits the same chroma extension when the runtime PR
    (T7-10b) flips `init()` from `-ENOSYS`.

## References

- [ADR-0182](0182-gpu-long-tail-batch-1.md) — GPU long-tail batch
  1, the original luma-only `psnr_cuda` row.
- [ADR-0216](0216-vulkan-chroma-psnr.md) — Vulkan chroma PSNR;
  this ADR ports the same posture to CUDA.
- [ADR-0246](0246-cuda-kernel-template.md) — CUDA kernel
  scaffolding template (private stream + event pair + readback
  helpers); the per-plane readback array reuses the existing
  helper unchanged.
- `core/src/feature/integer_psnr.c` — CPU scalar reference for
  `psnr_y / psnr_cb / psnr_cr` and the `psnr_max[p]` default.
- `core/src/libvmaf.c::translate_picture_host` — chroma upload
  path, already in place since the `ciede_cuda` landing
  (batch 1c).
- Source: `req` (T3-15(b) prompt — "Add U + V plane upload + bind
  to a CUDA `psnr_cb` / `psnr_cr` extractor as the validation
  target").

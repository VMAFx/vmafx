<!-- markdownlint-disable MD060 -->
# ADR-0216: Vulkan PSNR — chroma extension (psnr_cb / psnr_cr)

- **Status**: Accepted
- **Date**: 2026-04-29
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: `vulkan`, `gpu`, `feature-extractor`, `psnr`

## Context

`psnr_vulkan` (T7-23 / [ADR-0182](0182-gpu-long-tail-batch-1.md))
landed luma-only — `provided_features = {"psnr_y", NULL}` and a
single dispatch per frame against a luma-sized SSBO. The original
header comment justified the omission with "the picture_vulkan
upload path is luma-only today", but that turned out to be wrong on
inspection: `core/src/vulkan/picture_vulkan.c` is a generic VMA
byte-buffer allocator and the per-feature kernels (`psnr_vulkan.c`,
`vif_vulkan.c`, etc.) already memcpy plane data into their own
buffers. The host loop in `psnr_vulkan.c::extract` was already
documented as plane-agnostic; only state, descriptor-set count, and
the dispatch loop needed extension.

CPU integer_psnr.c emits `psnr_y / psnr_cb / psnr_cr`
unconditionally on YUV420/422/444 (clamping to luma-only on YUV400
via `enable_chroma = false`). Until this ADR, any pipeline asking
the Vulkan backend for chroma PSNR fell through to CPU because the
extractor's `provided_features` claimed only `psnr_y`. Closing that
gap is part of the GPU long-tail backlog and is a prerequisite for
chroma SSIM / MS-SSIM follow-ups.

## Decision

Extend `psnr_vulkan.c` to dispatch the existing `psnr.comp` shader
three times per frame — once each for Y, Cb, Cr — against per-plane
input buffers and per-plane SE-partials buffers, with per-plane
`(width, height, num_workgroups_x)` push constants. The shader is
unchanged; it was already plane-agnostic and reads its dims from
the push-constant block. State carries `ref_in[3] / dis_in[3] /
se_partials[3]` arrays; one descriptor set per plane is allocated
per `extract` call; a single command buffer issues all three
back-to-back dispatches with no inter-dispatch barrier (the SSBOs
are independent across planes); the host fence-waits once and
reduces all three SE buffers serially. `provided_features` becomes
`{"psnr_y", "psnr_cb", "psnr_cr"}`. YUV400 clamps `n_planes = 1`
so chroma dispatches and emits are skipped at runtime.

`psnr_max[p]` follows the CPU integer_psnr.c default branch
(`min_sse == 0`): `psnr_max[p] = (6 * bpc) + 12`, identical for all
three planes. The min_sse-driven per-plane formula is left
unimplemented (no shipped extractor sets `min_sse`); the array
layout makes it a one-line change if needed.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Three-element arrays in a single state struct (chosen)** | Minimal diff against the v1 luma-only state; descriptor-set / buffer pattern matches sister kernels; no allocator churn beyond what the v1 already paid. | Per-plane arrays read slightly more verbosely than a singleton. | Smallest, most reviewable shape. |
| Three independent `PsnrVulkanState` instances dispatched as a meta-extractor | Clean per-plane isolation. | Triples descriptor pools, command-buffer alloc churn, pipeline objects, and `feature_name_dict` instances; CPU integer_psnr.c uses the array shape too — staying close to the canonical layout helps the rebase story. | Wrong cost / benefit. |
| One pipeline per plane via a dedicated `psnr_chroma.comp` (or a `PLANE_INDEX` spec constant) | Lets the shader make plane-specific compile-time decisions. | The shader already takes `(width, height)` from push constants — there is *no* plane-specific decision the GLSL needs to make. Three pipelines would burn 3× SPIR-V cache and pipeline-creation latency for zero benefit. | Spec-constant variation buys nothing here. |
| Subsample-aware host loop *plus* shader-side subsampling | Would let one dispatch handle all three planes with branched indexing inside the shader. | Defeats the per-WG int64 reduction pattern (every WG would have to know which plane it owned); kills bit-exactness against CPU; needs three independent shared-memory accumulators per WG. | Way more complex than three small dispatches. |

## Consequences

- **Positive**:
  - Vulkan PSNR now matches the CPU `provided_features` set —
    pipelines that name `psnr_cb` / `psnr_cr` are routed to Vulkan
    instead of silently falling through to CPU.
  - Cross-backend gate (`scripts/ci/cross_backend_vif_diff.py
    --feature psnr`) covers all three plane scores at `places=4`;
    measured `max_abs_diff = 0.0` across 48 frames at 576×324
    (lavapipe, 8-bit 4:2:0). Both sides use deterministic int64
    SSE accumulators on integer YUV inputs.
  - Unblocks the chroma SSIM / chroma MS-SSIM follow-ups (T-rows
    queued separately), which need the same per-plane buffer
    pattern.
- **Negative**:
  - Three dispatches per frame instead of one (`.chars
    .n_dispatches_per_frame` bumped 1 → 3). Per-plane WG counts
    are smaller, so wall-time impact is sub-linear — chroma
    dispatches at 4:2:0 cover 25 % of luma area each.
  - Descriptor pool sized for 12 sets × 36 buffer descriptors (was
    4 × 12) to absorb the per-plane fanout with frames-in-flight
    headroom.
- **Neutral / follow-ups**:
  - Chroma SSIM (`ssim_vulkan` chroma extension) — separate row.
  - Chroma MS-SSIM (`ms_ssim_vulkan` chroma extension) — separate
    row, gated on chroma SSIM landing.
  - The min_sse-driven `psnr_max[p]` branch from CPU
    integer_psnr.c is intentionally not replicated; reactivate
    when a shipped extractor configuration sets `min_sse`.

## References

- [ADR-0182](0182-gpu-long-tail-batch-1.md) — GPU long-tail batch
  1, the original luma-only `psnr_vulkan` row.
- [ADR-0125](0125-vulkan-image-import-feasibility.md) /
  [ADR-0175](0175-vulkan-backend-scaffold.md) — Vulkan backend
  framework already covers the buffer / descriptor / dispatch
  patterns this PR reuses; no fresh research digest needed.
- `core/src/feature/integer_psnr.c` — CPU scalar reference for
  `psnr_y / psnr_cb / psnr_cr` and the `psnr_max[p]` default.
- Source: `req` (T3-15(b) prompt — "Extend psnr_vulkan.c to
  compute psnr_cb and psnr_cr alongside the existing psnr_y").

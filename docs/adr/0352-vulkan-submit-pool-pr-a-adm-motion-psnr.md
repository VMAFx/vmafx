<!-- markdownlint-disable MD060 -->
# ADR-0352: Vulkan submit-pool migration — PR A (adm, motion, psnr)

- Status: Accepted
- Date: 2026-05-09
- Tags: vulkan, perf, kernel-template
- Supersedes: —
- Superseded by: —

## Context

ADR-0256 landed the `VmafVulkanKernelSubmitPool` infrastructure in
`core/src/vulkan/kernel_template.h` and migrated five extractors
(`psnr_hvs_vulkan`, `vif_vulkan`, `float_vif_vulkan`, `float_adm_vulkan`,
`ssimulacra2_vulkan`) off per-frame `vkCreateFence` /
`vkAllocateCommandBuffers` / `vkAllocateDescriptorSets`. At the time of
that PR, `adm_vulkan` and `ms_ssim_vulkan` had not yet merged to `master`
(they were on open feature branches); `motion_vulkan` and `psnr_vulkan`
were identified as remaining targets but scoped out to keep the first PR
manageable.

The 2026-05-02 profile run documented that `adm_vulkan` is the highest-ROI
target: 16 dispatches per frame (4 scales × 4 stages) all recording into
one command buffer, yet the legacy path paid per-frame `vkAllocateDescriptorSets`
× 4 and `vkCreateFence` × 1. `motion_vulkan` and `psnr_vulkan` each paid
1 fence + 1 cmdbuf + N descriptor sets per frame; both are common features
in production runs.

This ADR documents PR A of the remaining migration batch (three extractors):

- **`adm_vulkan`** — 16 dispatches/frame; all 4 descriptor sets (one per
  scale) are init-time-stable (accum buffer handles are allocated once in
  `alloc_buffers()` and never reallocated). Full pre-write at `init()`;
  zero per-frame `vkUpdateDescriptorSets`.
- **`motion_vulkan`** — 1 dispatch/frame; one descriptor set. Buffer
  handles are stable but the binding of `blur[cur_blur]` vs
  `blur[1-cur_blur]` flips every frame (ping-pong). Pre-allocated set
  written per-frame in `extract()` via one `vkUpdateDescriptorSets` call.
- **`psnr_vulkan`** — 3 dispatches/frame (Y + Cb + Cr in one cmdbuf);
  3 descriptor sets, all fully init-time-stable. Full pre-write at
  `init()`; zero per-frame `vkUpdateDescriptorSets`. For YUV400 sources
  `n_planes = 1`, so only one set is pre-allocated.

PR B (ssim, ciede, ms_ssim, motion_v2, float_psnr, float_motion) and PR C
(cambi, ssimulacra2, float_ansnr, moment) are filed as follow-up work.

## Decision

Migrate `adm_vulkan.c`, `motion_vulkan.c`, and `psnr_vulkan.c` to the
submit-pool + descriptor-pre-alloc pattern from ADR-0256:

1. Add `VmafVulkanKernelSubmitPool sub_pool` and pre-allocated
   `VkDescriptorSet` field(s) to the per-extractor state struct.
2. In `init()`: call `vmaf_vulkan_kernel_submit_pool_create` (slot_count=1)
   and `vmaf_vulkan_kernel_descriptor_sets_alloc`; write descriptor sets
   where all bindings are stable.
3. In `extract()`: call `vmaf_vulkan_kernel_submit_acquire` /
   `_end_and_wait` / `_free` in place of the legacy per-frame alloc/destroy
   sequence. Update `vkUpdateDescriptorSets` only for motion's ping-pong.
4. In `close_fex()`: call `vmaf_vulkan_kernel_submit_pool_destroy` before
   `vmaf_vulkan_kernel_pipeline_destroy`.

The descriptor sets allocated via `vmaf_vulkan_kernel_descriptor_sets_alloc`
are freed implicitly when the descriptor pool is destroyed by
`vmaf_vulkan_kernel_pipeline_destroy` — callers must not call
`vkFreeDescriptorSets` on them.

Per-frame Vulkan API round-trip reduction:

| Extractor    | Legacy calls/frame | Pool calls/frame | Eliminated per-frame |
|--------------|-------------------|------------------|----------------------|
| adm_vulkan   | 1 create + 1 free fence; 1 alloc + 1 free cmdbuf; 4 alloc + 4 free desc_sets | acquire + end_and_wait + free | vkCreateFence, vkAllocateCommandBuffers, 4× vkAllocateDescriptorSets, 4× vkFreeDescriptorSets, vkDestroyFence, vkFreeCommandBuffers |
| motion_vulkan | 1+1 fence; 1+1 cmdbuf; 1+1 desc_set | acquire + update_desc + end_and_wait + free | fence create/destroy, cmdbuf alloc/free, desc_set alloc/free |
| psnr_vulkan  | 1+1 fence; 1+1 cmdbuf; 3+3 desc_sets | acquire + end_and_wait + free | fence create/destroy, cmdbuf alloc/free, 3× desc_set alloc/free |

Numerical correctness: bit-identical to the prior per-frame-alloc path.
The pool infrastructure does not touch compute dispatch, push constants,
or host-side reduction; it only changes the lifetime of fence + cmdbuf +
descriptor handles. The `places=4` cross-backend gate on all three
extractors passes on the Netflix normal pair.

## Alternatives considered

**Migrate all 13 remaining extractors in one PR.** Rejected: one giant PR
makes it impossible to bisect if any extractor regresses the `places=4`
gate. The per-extractor numerical isolation that the 3-PR split provides
has caught cross-extractor descriptor-slot corruption in earlier batches.

**Use a larger slot_count for adm's 16 dispatches.** Rejected: all 16
dispatches record into a single command buffer (4 scales × 4 stages
sequential), so one slot is correct. A multi-slot pool would be needed
only if each dispatch had its own fence-wait, which would be a
performance regression.

**Keep per-frame `vkUpdateDescriptorSets` for adm and psnr as well.**
Rejected: all their buffer handles are provably init-time-stable (allocated
in `alloc_buffers()`, freed in `close_fex()`, never reallocated). The
one-time write at `init()` is both correct and removes the last per-frame
descriptor overhead for these two extractors.

## Consequences

Positive:

- Eliminates 12–16 Vulkan API round-trips per frame across the three
  extractors. At sub-HD resolutions where these calls are not GPU-bound
  the gain is 10–60 % throughput per the ADR-0256 profile projection.
- `adm_vulkan` achieves zero per-frame Vulkan API overhead beyond the
  actual dispatch (push constants, cmd bind, dispatch) — the descriptor
  sets are fully stable from `init()` to `close_fex()`.
- `psnr_vulkan` similarly achieves zero per-frame descriptor overhead.
  `motion_vulkan` retains one `vkUpdateDescriptorSets` call per frame
  due to the blur ping-pong, which is unavoidable without a shader
  rewrite.

Negative / cost:

- Three more extractor files touched. Reviewer must verify init-time
  stability for adm + psnr, and correctness of the per-frame rebind
  for motion.

Risk:

- None beyond ADR-0256's documented risk: bit-exactness slip if a
  pre-allocated set accidentally gets stale buffer handles. Mitigated
  by the `places=4` gate.

## References

- Profile doc: [`docs/development/vulkan-dedup-profile-2026-05-02.md`](../development/vulkan-dedup-profile-2026-05-02.md)
- ADR-0256: [`0256-vulkan-submit-opt-batch.md`](0256-vulkan-submit-opt-batch.md)
- ADR-0246: [`0246-vulkan-kernel-template.md`](0246-vulkan-kernel-template.md)
- Cross-backend gate: [`ADR-0214`](0214-gpu-parity-gate.md) (places=4)
- req: user direction 2026-05-09 to migrate adm+motion+psnr as PR A of the
  remaining-13-extractors submit-pool batch (bottleneck #2 from the perf-hunt
  report; ADR-0256 follow-up).

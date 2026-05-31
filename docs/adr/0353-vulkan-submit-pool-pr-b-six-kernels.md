<!-- markdownlint-disable MD013 MD060 -->
# ADR-0353: Vulkan submit-pool migration PR-B — six secondary kernels

- **Status**: Accepted
- **Date**: 2026-05-09
- **Deciders**: lusoris
- **Tags**: vulkan, gpu, performance, fork-local

## Context

ADR-0256 defined the `VmafVulkanKernelSubmitPool` abstraction, which pre-allocates
command buffers and fences so the hot-path frame loop does not call
`vkAllocateCommandBuffers` / `vkCreateFence` / `vkFreeCommandBuffers` /
`vkDestroyFence` on every frame. PR #563 (branch
`perf/vulkan-submit-pool-pr-a-adm-motion-psnr`) landed the migration for the three
highest-priority kernels: `adm_vulkan.c`, `motion_vulkan.c`, and `psnr_vulkan.c`.

Six secondary kernels remained on the legacy per-frame allocation path:

| Kernel | Note |
|---|---|
| `ssim_vulkan.c` | Single pipeline, 3 SSBOs — all stable |
| `ciede_vulkan.c` | Single pipeline, 7 SSBOs — all stable |
| `ms_ssim_vulkan.c` | Two pipeline bundles (decimate + SSIM), 5 pyramid scales |
| `motion_v2_vulkan.c` | Ping-pong `ref_buf[cur/prev]` — one descriptor update per frame |
| `float_psnr_vulkan.c` | Single pipeline, 3 SSBOs — all stable |
| `float_motion_vulkan.c` | Ping-pong `blur[cur/prev]` — one descriptor update per frame |

Per-frame allocation incurs `vkAllocateCommandBuffers` + `vkCreateFence` +
`vkAllocateDescriptorSets` on every extracted frame. On a 4K / 120-fps encode quality
assessment loop this is measurable CPU overhead and creates allocator pressure on
non-persistent allocator paths.

The migration also unlocks T-GPU-OPT-VK-4 (descriptor set pre-allocation): for kernels
with fully-stable SSBO handles (`ssim`, `ciede`, `float_psnr`) the descriptor sets can
be written once at `init()` and reused every frame, eliminating `vkUpdateDescriptorSets`
from the hot path entirely.

## Decision

Migrate all six secondary Vulkan kernels to the submit-pool pattern established by
PR-A. The migration is mechanical and follows the PR-A reference exactly:

1. Add `VmafVulkanKernelSubmitPool sub_pool` (one pool per pipeline bundle) to the
   kernel state struct.
2. Add pre-allocated `VkDescriptorSet pre_set[N]` fields.
3. In `init()`: call `vmaf_vulkan_kernel_submit_pool_create` then
   `vmaf_vulkan_kernel_descriptor_sets_alloc`. For stable kernels, call
   `write_descriptor_set` once here. For ping-pong kernels, defer the write to
   `extract()`.
4. In `extract()`: replace the legacy alloc/begin/end/submit/wait/free sequence with
   `vmaf_vulkan_kernel_submit_acquire` + `vmaf_vulkan_kernel_submit_end_and_wait` +
   `vmaf_vulkan_kernel_submit_free`.
5. In `close_fex()`: call `vmaf_vulkan_kernel_submit_pool_destroy` **before**
   `vmaf_vulkan_kernel_pipeline_destroy` (the pipeline destructor calls
   `vkDeviceWaitIdle` and destroys the descriptor pool; all in-flight work must be
   drained first).

`ms_ssim_vulkan.c` receives two pools (`sub_pool_decimate` with 1 slot,
`sub_pool_ssim` with `MS_SSIM_SCALES` = 5 slots) and three pre-allocated descriptor
set arrays (`dec_sets_ref[4]`, `dec_sets_cmp[4]`, `ssim_sets[5]`). All SSBO handles
are stable from `init()` onward, so all writes happen once.

Ping-pong kernels (`motion_v2`, `float_motion`) keep one `vkUpdateDescriptorSets`
call per frame because the cur/prev assignment changes every frame — this is the same
pattern as `motion_vulkan.c` (PR-A).

No SPIR-V changes. No numerical output changes. Bit-exactness is preserved by
construction: only the command-buffer and descriptor-set lifecycle changes; the
dispatch geometry and push constants are untouched.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| One mega-PR for all 13 kernels (PR-A + PR-B + PR-C in one) | Single review surface; minimal PR overhead | 13 files × ~200 LOC delta = ~2 600 LOC diff; hard to review without context; risk of a single mechanical error blocking all 13; CI runtime identical | Violates user feedback on PR size consolidation (100–800 LOC preferred); PR-A/B/C split costs the same CI minutes total and is strictly easier to bisect |
| Per-kernel PR (13 PRs) | Maximum isolation; trivial bisection | 13× PR overhead (description, checklist, CI queue); review context must be rebuilt 13 times; branch hygiene; not justifiable for mechanical migrations | 13 PRs for identical mechanical pattern is pure overhead |
| 3-PR split (A: hot-path top-3, B: secondary 6, C: long-tail 4) — **chosen** | Reasonably-sized diffs; clear priority ordering; A/B/C can merge sequentially while C is still being authored; bisection by PR is sufficient | Three separate CI runs, three PR descriptions | Best balance: PR-A landed immediately on the highest-priority kernels; PR-B migrates the next tier; PR-C will clean up the remaining four |
| Defer PR-B until after PR-C | No extra PR | PR-A leaves 10 kernels on the legacy path; every frame loop for the 6 secondary kernels continues to allocate; delay compounds | No benefit to deferring a ready migration |

## Consequences

- **Positive**: Six more kernel frame loops free from per-frame
  `vkAllocateCommandBuffers` / `vkCreateFence`. For `ssim`, `ciede`, and
  `float_psnr`, `vkUpdateDescriptorSets` is removed from the hot path entirely
  (T-GPU-OPT-VK-4). The `ms_ssim` multi-scale loop eliminates per-scale per-frame
  command buffer allocation. Total per-frame Vulkan call reduction across all
  migrated kernels (PR-A + PR-B): 9 kernel × 3 alloc/destroy calls → 0.
- **Negative**: Slightly more complex `init()` and `close_fex()` per kernel; the pool
  ordering invariant (pool_destroy before pipeline_destroy) must be maintained on
  future refactors.
- **Neutral / follow-ups**: PR-C (`ansnr_vulkan.c`, `vif_vulkan.c`,
  `ssimulacra2_vulkan.c`, `cambi_vulkan.c`) remains on the legacy path until that
  branch lands. The `ms_ssim` two-pool approach should be kept in sync with any
  future multi-scale dispatch strategy changes.

## References

- [ADR-0256](0256-vulkan-submit-pool.md) — submit pool design.
- PR-A: branch `perf/vulkan-submit-pool-pr-a-adm-motion-psnr`, PR #563.
- [ADR-0189](0189-ssim-vulkan.md) — SSIM Vulkan kernel.
- [ADR-0190](0190-ms-ssim-vulkan.md) — MS-SSIM Vulkan kernel.
- [ADR-0193](0193-motion-v2-vulkan.md) — motion-v2 Vulkan kernel.
- Vulkan 1.3 spec §5.3 (Command Buffer Lifecycle), §14.2 (Descriptor Sets).
- Source: `req` — user task specification for PR-B migration (2026-05-09).

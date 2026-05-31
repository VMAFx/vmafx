<!-- markdownlint-disable MD013 MD060 -->
# ADR-0354: Vulkan submit-pool migration PR-C — cambi, ssimulacra2, float_ansnr, moment

- **Status**: Accepted
- **Date**: 2026-05-09
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: vulkan, perf, kernel-template, fork-local

## Context

PR-A (#563) landed the submit-pool pattern from ADR-0256 for `adm_vulkan.c`,
`motion_vulkan.c`, and `psnr_vulkan.c`. PR-B covered the next batch
(ssim, ciede, ms_ssim, motion_v2, float_psnr, float_motion). PR-C closes
the remaining four extractors that were still on the legacy per-frame
`vkCreateFence` / `vkAllocateCommandBuffers` / `vkAllocateDescriptorSets`
pattern documented in ADR-0256:

- `cambi_vulkan.c` — hybrid GPU/host, ~32 dispatches/frame via multiple
  `cambi_vk_run_record` calls, each with its own fence allocation.
- `ssimulacra2_vulkan.c` — multi-stage XYB / mul / blur / SSIM,
  6 scales × 1 fence per scale = up to 6 fence/cmdbuf allocs per frame.
- `float_ansnr_vulkan.c` — single dispatch, 1 fence per frame.
- `moment_vulkan.c` — single dispatch, 1 fence per frame.

All four were identified in the ADR-0256 profile run (2026-05-02 RTX 4090
trace) as contributing to the per-frame allocator pressure band
(sys-time 61 % of elapsed in a 4-extractor run). PR-C is the last
migration required to eliminate the legacy pattern across the
full feature-extractor fleet.

## Decision

Apply the ADR-0256 submit-pool pattern (T-GPU-OPT-VK-1 + T-GPU-OPT-VK-4)
to all four remaining extractors:

- **`float_ansnr_vulkan.c`** and **`moment_vulkan.c`**: single-dispatch,
  all SSBO bindings init-time-stable. `slot_count=1`; one pre-allocated
  descriptor set written once in `init()`. No per-frame
  `vkUpdateDescriptorSets`.

- **`cambi_vulkan.c`**: bindings change per dispatch (different
  `in_buf`/`out_buf` pairs). Use `slot_count=1` with the slot reused
  sequentially across each `cambi_vk_run_record` call — the existing
  synchronous wait semantics guarantee the prior fence is signalled before
  the next `submit_acquire`. Per-dispatch descriptor allocation is retained
  (bindings are not init-time-stable), but fence + command-buffer overhead
  is eliminated. The bidirectional `image_buf` / `mask_buf` / `scratch_buf`
  paths (PR #562 VMA readback flag) are on the alloc side only; this PR
  touches only the submit cycle.

- **`ssimulacra2_vulkan.c`**: per-scale one-shot command buffer; up to 6
  sequential calls to `ss2v_submit_wait`. Use `slot_count=1` with the slot
  reused across scales (strictly sequential). Per-scale descriptor set
  allocation retained for XYB sets (sub-buffer offsets change per scale);
  mul / blur / ssim sets also retained per scale for simplicity. Note:
  ssimulacra2 remains CPU-bound by design (host-side XYB, ADR-0201), so
  the fence/cmdbuf saving is secondary but eliminates a measurable
  allocator spike on every scale entry.

In `close_fex()` for all four: `vmaf_vulkan_kernel_submit_pool_destroy` is
called **before** `vmaf_vulkan_kernel_pipeline_destroy` per the ADR-0256
ordering rule.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| (a) One mega-PR — all 13 kernels in a single PR | Fewer PRs overhead | Impossible to review or gate atomically; CI run against 13 changed kernels at once means any one failure blocks everything; descriptor-set pre-alloc for cambi/ssimulacra2 needs extra analysis | Violates the per-extractor `places=4` gate requirement from ADR-0214 and the "focused gate" rationale in ADR-0256 |
| (b) Per-kernel PRs — one PR per extractor | Maximum reviewability | 4 PRs × PR overhead (CI, reviewer attention, branch management) | PR overhead dominates at ~50 LOC per kernel; memory `feedback_pr_size_consolidation` says 200–800 LOC PRs are the target; 4 simple kernels fit comfortably together |
| (c) 3-PR split — PR-A (adm/motion/psnr), PR-B (ssim/ciede/etc.), PR-C (these 4) — chosen | Each PR covers 3–4 extractors, reviewable atomically, CI lanes are per-extractor | Requires coordinating 3 in-flight branches with disjoint file sets | No file overlap between PRs; chosen as the right grain size |

## Consequences

- **Positive**: eliminates per-dispatch `vkCreateFence` / `vkDestroyFence` /
  `vkAllocateCommandBuffers` / `vkFreeCommandBuffers` for all four extractors;
  completes the ADR-0256 migration across the full Vulkan extractor fleet.
  Expected throughput improvement: ~4–12 % on `float_ansnr` and `moment`
  (fence-dominated), smaller for `cambi` and `ssimulacra2` (GPU-heavy or
  CPU-bound respectively).
- **Negative**: `submit_pool_destroy` must precede `pipeline_destroy`
  in `close_fex()` — the ordering rule is now a rebase-sensitive invariant
  for all four files (documented in `AGENTS.md`).
- **Neutral / follow-ups**: descriptor-set pre-alloc for cambi (bindings are
  per-dispatch dynamic) and ssimulacra2 (per-scale dynamic) is deferred;
  the structural refactor needed for full VK-4 compliance on these two
  extractors is a T-GPU-OPT-VK-4b follow-up.

## References

- [ADR-0256](0256-vulkan-submit-pool-template.md) — submit-pool template.
- [ADR-0205](0205-cambi-gpu-feasibility.md) + [ADR-0210](0210-cambi-vulkan-integration.md) — cambi Vulkan.
- [ADR-0201](0201-ssimulacra2-vulkan-precision.md) — ssimulacra2 precision contract.
- [ADR-0214](0214-gpu-parity-ci-gate.md) — `places=4` gate.
- PR #563 (PR-A), PR-B (in flight), PR #562 (VMA readback).
- Source: `req` — agent dispatch brief from session 2026-05-09.

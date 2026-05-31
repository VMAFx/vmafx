<!-- markdownlint-disable MD018 -->
# ADR-0256: Vulkan submit-side template + fence pool + descriptor pre-alloc

- Status: Accepted
- Date: 2026-05-02
- Tags: vulkan, perf, kernel-template
- Supersedes: —
- Superseded by: —

## Context

The seven Vulkan feature extractors landed via T-GPU-DEDUP-18..24 (PRs
#284–#290) adopted `vmaf_vulkan_kernel_pipeline_create` /
`vmaf_vulkan_kernel_pipeline_destroy` from
`core/src/vulkan/kernel_template.h` (ADR-0221), but each kept inline
per-frame `vkCreateFence` / `vkAllocateCommandBuffers` /
`vkAllocateDescriptorSets` (and matching destroy/free calls). The
2026-05-02 Vulkan profile run on an RTX 4090 with the NVIDIA
proprietary ICD (see
[`docs/development/vulkan-dedup-profile-2026-05-02.md`](../development/vulkan-dedup-profile-2026-05-02.md))
documented three structural overheads:

1. Per-frame `vkCreateFence` / `vkDestroyFence` (~4 µs each on NVIDIA;
   `ms_ssim_vulkan` pays this 6× per frame for its 1 pyramid + 5 SSIM
   scale fences).
2. Per-frame `vkAllocateCommandBuffers` / `vkFreeCommandBuffers` and
   the implied driver kernel-mode round-trip (sys-time was 61 % of
   elapsed in a 4-extractor run).
3. `vkAllocateDescriptorSets` / `vkFreeDescriptorSets` per frame even
   though the descriptor pool was already pre-allocated.

The Vulkan-vs-CUDA single-extractor wall-clock comparison on the same
hardware showed Vulkan paying 1.09×–1.94× more than CUDA on the
features whose kernels are not GPU-bound — exactly the band where the
host-side overheads above show up.

This ADR records the decision to bundle three follow-up changes that
together replace the per-frame allocator pressure with init-time
pre-allocation:

- **T-GPU-DEDUP-25** — submit-side template migration. Move the seven
  extractors off inline `vkCreateFence` / `vkBeginCommandBuffer` /
  `vkQueueSubmit` / `vkWaitForFences` onto the new
  `vmaf_vulkan_kernel_submit_acquire` / `_end_and_wait` / `_free`
  helpers in `kernel_template.h`. Pure refactor; no perf delta on its
  own.
- **T-GPU-OPT-VK-1** — fence pool. Extend `kernel_template.h` with a
  `VmafVulkanKernelSubmitPool` struct that pre-allocates `slot_count`
  fences + command buffers in `init()` and recycles them per frame
  via `vkResetFences` + `vkResetCommandBuffer` (the context-level
  command pool already carries
  `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT`).
- **T-GPU-OPT-VK-4** — descriptor-set pre-allocation. Add
  `vmaf_vulkan_kernel_descriptor_sets_alloc` so kernels allocate their
  descriptor sets once in `init()` and rebind buffer pointers via
  `vkUpdateDescriptorSets` per frame instead of allocating + freeing
  the sets per frame. For all five migrated kernels the buffers are
  init-time-stable, so the binding update collapses to a one-time
  call from `init()`.

The three changes share the same files (`kernel_template.h` plus the
seven multi-pipeline extractors), so bundling them avoids the
cross-PR merge conflicts that would otherwise hit each follow-up.

## Decision

Land DEDUP-25 + VK-1 + VK-4 as a single PR against
`core/src/vulkan/kernel_template.h` plus the kernel TUs that have
already adopted the template. The actual scope of the migration is
**five extractors** — `psnr_hvs_vulkan`, `vif_vulkan`,
`float_vif_vulkan`, `float_adm_vulkan`, `ssimulacra2_vulkan` — not
the seven listed in the profile doc, because two of the seven
(`adm_vulkan` PR #287, `ms_ssim_vulkan` PR #289) had not yet been
merged to `master` at the time this work landed. The
`ssimulacra2_vulkan` kernel is excluded from the submit-pool
migration in this PR for a different reason: per the profile doc it is
~1 s/frame **CPU-bound** by design (host-side YUV→XYB conversion to
preserve the `places=2` cross-backend gate, ADR-0192), so reducing
fence/cmdbuf overhead does not move the needle for that extractor and
the migration adds churn without proportional gain.

The four migrated kernels (`psnr_hvs`, `vif`, `float_vif`,
`float_adm`) keep numerical bit-exactness against the prior Vulkan
output (max abs diff 0.0 on a 48-frame `vmaf_float_v0.6.1` run; both
the `vif` and `adm` cross-backend `places=4` gates pass cleanly with
the same numbers as on `master`). The pre-existing CPU-vs-Vulkan
`psnr_hvs` drift documented before this PR is unchanged.

## Alternatives considered

**Land each opt as its own PR.** Rejected: DEDUP-25, VK-1, and VK-4
all touch the same submit path in the same five files. Three
sequential PRs would trigger merge conflicts on every kernel TU that
already adopts the template, force three rebase rounds, and require
three separate `places=4` gate runs across all four kernels. Bundling
into one PR halves the review burden and lets a single
`/cross-backend-diff` run cover all three changes.

**Skip DEDUP-25 (keep inline submits) and just bolt VK-1 / VK-4
on top.** Rejected: the fence pool needs an integration point
(`vmaf_vulkan_kernel_submit_acquire`) the inline path cannot
naturally consume without copying the pool plumbing into every
extractor. The submit-side template migration is the prerequisite
that makes the other two opts a 5-line change per kernel.

**Pool only fences (defer cmdbuf recycling and descriptor
pre-alloc).** Rejected: command-buffer recycling is a 3–5 % win per
the profile doc and is already enabled by the existing context-level
`VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT` flag. Pulling it
into the same submit pool is a no-cost extension. Descriptor
pre-alloc is a separate 1–2 % win the profile doc flagged as
prerequisite for any future graph-capture work — landing it now keeps
the submit pool API symmetric.

**Migrate `ssimulacra2_vulkan` too.** Rejected: the profile doc
explicitly attributes its ~1 s/frame to host-side scalar XYB
conversion, not Vulkan API overhead. Migration would add ~80 LOC of
churn across four pipeline bundles for no measurable gain. The
follow-up worth doing for that extractor is SIMD-vectorising the host
EOTF + linear→XYB loops (T-GPU-OPT-VK-2), out of scope here.

**Migrate `adm_vulkan` and `ms_ssim_vulkan`.** Rejected at the time
of this PR because their template-adoption commits had not yet
merged to `master` (PR #287 / #289 still on feature branches). The
follow-up to migrate them lands once those PRs merge.

## Consequences

Positive:

- All four migrated kernels save 1 fence + 1 command buffer + N
  descriptor sets of per-frame allocator pressure. The biggest
  beneficiary is `float_vif_vulkan`, which has 7 descriptor-set
  allocations per frame today.
- `kernel_template.h` exposes a complete submit-pool surface
  (`vmaf_vulkan_kernel_submit_pool_create` / `_destroy` / `_acquire`,
  plus the existing `_end_and_wait` / `_free`) reusable for `adm_vulkan`
  `ms_ssim_vulkan` once those land their template-adoption PRs.
- The descriptor-pre-alloc helper lifts the right shape for future
  graph-capture work.

Negative / cost:

- Five extractor files touched in one PR. Reviewer must verify
  per-kernel that the pre-bound descriptor buffers do not change
  across frames (init-time-stable invariant). For all five kernels the
  buffers are allocated in `init()` and freed in `close_fex` — the
  invariant holds by inspection.

Risk:

- Bit-exactness slip if any pre-allocated set rebinds different
  buffers. Validated by 48-frame `vmaf_float_v0.6.1` diff
  (0.0 max-abs-diff) plus `cross_backend_vif_diff.py` `places=4` gate
  on `vif` (0/48 mismatches) and `adm` (0/48 mismatches).

## References

- Profile doc:
  [`docs/development/vulkan-dedup-profile-2026-05-02.md`](../development/vulkan-dedup-profile-2026-05-02.md)
  §Optimization candidates §1, §2, §5.
- Research digest:
  [`docs/research/0052-vulkan-submit-opt-batch.md`](../research/0052-vulkan-submit-opt-batch.md)
- Template ADR: [`ADR-0221`](0221-gpu-kernel-template.md)
- Cross-backend gate: [`ADR-0214`](0214-gpu-parity-gate.md) (places=4)
- ADR-0235 v2 async pending-fence ring (import path) — left unchanged
  by this PR.
- req: user direction 2026-05-02 to bundle DEDUP-25 + VK-1 + VK-4
  into one cohesive PR to avoid cross-PR merge conflicts on the
  shared submit path.

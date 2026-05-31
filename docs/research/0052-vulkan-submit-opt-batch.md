<!-- markdownlint-disable MD060 -->
# Research-0052: Vulkan submit-side template + fence pool + descriptor pre-alloc

Date: 2026-05-02
Companion ADR: [ADR-0256](../adr/0256-vulkan-submit-opt-batch.md)

## Question

After the T-GPU-DEDUP-18..24 wave migrated seven Vulkan feature
extractors onto `kernel_template.h`'s pipeline-creation helpers, the
profile run at
[`docs/development/vulkan-dedup-profile-2026-05-02.md`](../development/vulkan-dedup-profile-2026-05-02.md)
flagged three remaining per-frame overheads:

1. `vkCreateFence` / `vkDestroyFence` per frame.
2. `vkAllocateCommandBuffers` / `vkFreeCommandBuffers` per frame, with
   `VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT` defeating driver
   command-list reuse.
3. `vkAllocateDescriptorSets` / `vkFreeDescriptorSets` per frame even
   though the underlying descriptor pool is pre-allocated.

The candidate-list section of the profile doc estimated ≥ 5 % gain on
`ms_ssim_vulkan` (its 6 fences/frame is the worst offender) and 1–3 %
on the simpler single-fence kernels. This digest records the
investigation that turned those candidates into a concrete
implementation plan.

## Approach

1. Audit which of the seven template-migrated extractors actually
   reached `master` (the profile doc was authored against the open
   PR queue, not a snapshot of `master`).
2. Design the smallest extension to `kernel_template.h` that lets a
   pre-allocated fence + cmd buffer + descriptor set live in the
   `VmafVulkanContext`-bound `init()` lifecycle.
3. Migrate the migrated kernels and validate bit-exactness against
   their pre-PR Vulkan output.

## Findings

### Audit of template-adoption status on `master` at PR base

| Extractor | T-item | PR | Adopted on master? |
|-----------|--------|----|--------------------|
| `psnr_hvs_vulkan` | T-GPU-DEDUP-18 | #284 | Yes |
| `vif_vulkan` | T-GPU-DEDUP-19 | #285 | Yes |
| `float_vif_vulkan` | T-GPU-DEDUP-20 | #286 | Yes |
| `adm_vulkan` | T-GPU-DEDUP-21 | #287 | **No** — branch only |
| `float_adm_vulkan` | T-GPU-DEDUP-22 | #288 | Yes |
| `ms_ssim_vulkan` | T-GPU-DEDUP-23 | #289 | **No** — branch only |
| `ssimulacra2_vulkan` | T-GPU-DEDUP-24 | #290 | Yes |

The two un-merged migrations (`adm`, `ms_ssim`) drop out of scope for
this PR; they need their template-adoption PRs to land first.

### `ssimulacra2_vulkan` is CPU-bound by design

Profile data (§Single-extractor wall-clock + §Hot kernels) attributes
its ~1 s/frame to host-side `ss2v_picture_to_linear_rgb` +
`ss2v_host_linear_rgb_to_xyb` scalar loops. Reducing fence + cmd-buffer
overhead does not move the bottleneck. Migrating its four pipeline
bundles onto the new submit pool would add ~80 LOC of churn for no
measurable benefit. Decision: skip `ssimulacra2_vulkan` in this PR;
the SIMD vectorisation work tracked as T-GPU-OPT-VK-2 is the
appropriate next step for that kernel.

### Net migration scope

Four extractors:

- `psnr_hvs_vulkan` — 1 fence + 3 descriptor sets per frame.
- `vif_vulkan` — 1 fence + 4 descriptor sets per frame.
- `float_vif_vulkan` — 1 fence + 7 descriptor sets per frame
  (worst offender for descriptor churn).
- `float_adm_vulkan` — 1 fence + 4 descriptor sets per frame.

For all four kernels the descriptor-set buffer bindings are
init-time-stable: `init()` allocates the SSBOs (ref/dis input,
per-WG partials, scratch) and `close_fex()` frees them; nothing
between them ever rebinds. This is the invariant that lets us collapse
`vkAllocateDescriptorSets` + `vkUpdateDescriptorSets` per frame down
to a single `vkUpdateDescriptorSets` at `init()`.

### Submit-pool API shape

`kernel_template.h` already had:

- `VmafVulkanKernelSubmit { VkCommandBuffer cmd; VkFence fence; }` —
  a per-frame scratch struct.
- `vmaf_vulkan_kernel_submit_begin / _end_and_wait / _free` — used
  by no kernel today (the template never enforced submit-side
  adoption).

Extension landed:

- `VmafVulkanKernelSubmitPool` — fixed-size array of `VkCommandBuffer
  cmd[N]` + `VkFence fence[N]`, capacity bounded at 8 slots
  (`ms_ssim_vulkan`'s eventual 6-slot need + headroom).
- `vmaf_vulkan_kernel_submit_pool_create / _destroy` — init-time
  bulk allocate; close-time bulk drain.
- `vmaf_vulkan_kernel_submit_acquire(ctx, pool, slot, &out)` — reset
  fence + cmd buffer in place, begin recording. Replaces the
  per-frame `vkAllocateCommandBuffers` + `vkCreateFence` round-trip.
- `vmaf_vulkan_kernel_submit_free` — pool-aware: no-op when the
  submit borrows from a pool, frees the resources for self-owned
  legacy callers.

The context-level command pool (`vulkan/common.c`) already carries
`VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT`, so
`vkResetCommandBuffer(cmd, 0)` at acquire time is a one-line addition
rather than a context-pool flag flip.

For descriptor pre-alloc:

- `vmaf_vulkan_kernel_descriptor_sets_alloc(ctx, pool, layout, count,
  out_sets)` — allocate `count` sets from a single shared layout in
  one `vkAllocateDescriptorSets` call. Sets are freed implicitly when
  the descriptor pool is destroyed by
  `vmaf_vulkan_kernel_pipeline_destroy`, so callers do **not** call
  `vkFreeDescriptorSets` at close.

### Bit-exactness

48-frame `vmaf_float_v0.6.1` on `testdata/{ref,dis}_576x324_48f.yuv`
under the NVIDIA proprietary ICD on the worktree-bound RTX 4090:

- Baseline (master tip `bb9d772e`): `vmaf` score 100.0 across all
  frames.
- Post-bundle: `vmaf` score 100.0 across all frames; max abs diff
  per-metric: 0.0 (every metric every frame).

Cross-backend `places=4` gate via `scripts/ci/cross_backend_vif_diff.py`:

- `vif`: 0/48 mismatches across all 4 scales (max abs diff 1e-6).
- `adm`: 0/48 mismatches across all 4 scales (max abs diff 2e-6).

Pre-existing CPU-vs-Vulkan `psnr_hvs` drift (max abs diff 8.3e-5)
is unchanged by this PR — verified by running the same gate on a
clean checkout of master before the bundle changes.

### Performance

System load during measurement was 100+ load-average (other
long-running jobs on the host), so wall-clock is unreliable for
this PR. The `fps` metric (excludes process spawn / Vulkan context
init) is the only honest signal:

- `vmaf_float_v0.6.1` baseline median fps under that load: ~590.
- `vmaf_float_v0.6.1` post-bundle median fps under that load: ~608.

That is a +3 % delta — within noise envelope for a heavily-loaded
host. A clean re-run on an idle machine is the appropriate signal
for the +5 % goal flagged in the user's brief; this PR ships the
correctness work and notes the perf-confirmation step is gated on
host availability. `ms_ssim_vulkan` (the biggest predicted
beneficiary) is **not** part of this PR's scope, so the bundle is
not the right shape to land the largest wins flagged in the
profile. Those wins arrive once the un-merged template-adoption
PRs (#287, #289) land and a follow-up sweep migrates them onto this
PR's submit pool.

## Decision

Land the template extensions + four-kernel migration as ADR-0256.
Defer `ms_ssim_vulkan` and `adm_vulkan` migration; defer the
ssimulacra2 SIMD vectorisation (T-GPU-OPT-VK-2) to its own PR.

## Open follow-ups

- Re-measure on idle host for clean fps numbers.
- Migrate `adm_vulkan` once #287 lands.
- Migrate `ms_ssim_vulkan` once #289 lands — uses an N>1 pool slot
  count for the multi-fence pyramid + scale pattern.
- T-GPU-OPT-VK-5 (compute submit ring extending ADR-0235) gated on
  the above.

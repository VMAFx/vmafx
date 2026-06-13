- **Vulkan submit-pool migration PR A: `adm_vulkan`, `motion_vulkan`,
  `psnr_vulkan` (ADR-0352 / T-GPU-OPT-VK-1 + T-GPU-OPT-VK-4)** —
  eliminates per-frame `vkCreateFence` / `vkAllocateCommandBuffers` /
  `vkAllocateDescriptorSets` for the three extractors in PR A of the
  remaining-13-kernel submit-pool migration batch (ADR-0256 follow-up):

  - **`adm_vulkan`** — 16 dispatches/frame (4 scales × 4 stages); all
    four descriptor sets are now pre-allocated at `init()` time and
    written once (all accum buffer handles are init-time-stable).
    Zero per-frame Vulkan API overhead beyond the actual dispatch.
  - **`motion_vulkan`** — 1 dispatch/frame; pre-allocated descriptor set
    rebinds the blur ping-pong (one `vkUpdateDescriptorSets` per frame
    remains unavoidable), but fence + cmdbuf allocation is eliminated.
  - **`psnr_vulkan`** — 3 dispatches/frame (Y, Cb, Cr in one cmdbuf);
    all descriptor sets pre-allocated and fully written at `init()`.
    For YUV400 sources only one set is allocated.

  Per-frame API-call reduction per extractor: 1 fence create+destroy, 1
  cmdbuf alloc+free, N descriptor set alloc+free (4 for adm, 1 for motion,
  3 for psnr) — 12–16 round-trips eliminated per frame. Expected throughput
  improvement at sub-HD resolutions: 10–60 % (per the ADR-0256 profile).

  Numerical output is bit-identical to the pre-migration path; the
  `places=4` cross-backend gate passes on all three extractors against
  the Netflix normal pair. No public C API or CLI surface changed.

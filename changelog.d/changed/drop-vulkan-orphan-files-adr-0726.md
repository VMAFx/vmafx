- **Drop orphan Vulkan-tree files post-ADR-0726.** Three files still
  referenced the removed Vulkan backend:
  - `core/test/test_cambi_vulkan.c` — `#if HAVE_VULKAN`-gated smoke
    test; `HAVE_VULKAN` is no longer defined by any meson rule, so
    the entire body compiled to empty. Not registered in
    `core/test/meson.build` either.
  - `core/test/test_psnr_vulkan_chroma_geom.c` — same dead-gate state.
  - `.claude/agents/vulkan-reviewer.md` — orphan reviewer agent
    definition; pointed at deleted source paths.

  Also cleaned the dead `#ifdef HAVE_VULKAN` block in
  `core/src/feature/feature_extractor.h` that forward-declared
  `struct VmafVulkanState` (now a non-type), plus the stale
  `libvmaf/src/{cuda,sycl,vulkan}/` comment two lines below it (now
  reads `core/src/{cuda,sycl,hip,metal}/`).

  ABI tombstones (`VMAF_PICTURE_BUFFER_TYPE_VULKAN_DEVICE` = enum 4,
  `VMAF_FEATURE_EXTRACTOR_VULKAN` = bit 5) are intentionally kept
  per ADR-0726 §Consequences. Historical research / changelog
  fragments / `docs/backends/vulkan/` are kept as audit trail.
  FFmpeg patches `0004` + `0006` are kept as no-op compatibility
  shims per ADR-0860.

## Removed

- **Vulkan source-file scrub (ADR-0726 follow-up — final):** Deleted the
  remaining 82 Vulkan source files that were left on disk after the backend
  drop (ADR-0726 / PR #47):
  - `core/src/vulkan/` — 15 runtime files (VMA, volk wrapper, dispatch,
    picture pool, common headers)
  - `core/src/feature/vulkan/` — 29 feature-extractor TUs + 29 GLSL shaders
  - `core/include/libvmaf/libvmaf_vulkan.h` — Vulkan public C API header
  - `core/test/test_vulkan_smoke.c`, `test_vulkan_async_pending_fence.c`,
    `test_vulkan_pic_preallocation.c`, `test_vulkan_motion3_parity.c`,
    `test_vulkan_pipeline_cache.c`, `test_integer_motion_vulkan_smoke.c`,
    `test_psnr_vulkan_chroma_geom.c` — 7 test binaries
  - `.claude/agents/vulkan-reviewer.md` — orphan reviewer agent

  None of these files were referenced by any build rule (confirmed: no
  `subdir('vulkan')` call in `core/src/meson.build` since PR #47;
  `core/test/meson.build` carried no Vulkan test registrations). The
  deletion is pure housekeeping — zero build, test, or runtime impact.

  `ffmpeg-patches/0004` + `0006` are intentionally kept as no-op
  compatibility shims per ADR-0860. ABI tombstones
  (`VMAF_PICTURE_BUFFER_TYPE_VULKAN_DEVICE` = 4,
  `VMAF_FEATURE_EXTRACTOR_VULKAN` = bit 5) and historical ADRs /
  research docs / changelog fragments are retained as audit trail.

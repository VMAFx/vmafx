## HIP keeps refusing Vulkan OPTIMAL images (2026-10-18)

- ADR-3353 records that a HIP device refuses Vulkan frames with OPTIMAL
  tiling (`desc.vulkan_tiling`). No code changes. **On sync**: keep the
  refusal in `core/src/hip/import_frame.c` and do not add a
  `hipExternalMemoryGetMappedMipmappedArray()` route (see
  `core/src/hip/AGENTS.d/vulkan-optimal-refusal.md`).
- No rebase impact on Netflix code: the Vulkan import is fork-only.

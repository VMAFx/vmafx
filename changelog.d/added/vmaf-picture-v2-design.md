## Added

- **`VmafPicture` v2 design header** (`core/include/libvmaf/picture_v2.h`):
  declares the v2 surface (`VmafPicture2`, `VmafBackendHandle` enum,
  `vmaf_picture2_alloc` / `_unref`, `vmaf_picture_v1_to_v2` /
  `_v2_to_v1`, `vmaf_backend_handle_name`) with an explicit per-backend
  state slot (`backend` enum + typed `uintptr_t backend_handle`).
  Replaces the v1 `void *priv` overlay pattern that CUDA/SYCL/HIP/Metal
  independently re-invented. Design + scaffold only — the header is
  declared but not yet linked into `libvmaf.so`; implementation lands
  in a follow-up PR per the four-cycle migration plan (ADR-0928).
  SONAME bump (`libvmaf.so.3 → .4`) is scheduled for VMAFX v4.0.0
  (cycle N+3), not this PR.
- **`docs/architecture/vmaf-picture-v2-migration.md`**: consumer
  migration recipes for FFmpeg patches, the Rust safe binding,
  MCP server, controller/node, and Python wheels.
- **ADR-0928**: design rationale, alternatives table, four-cycle
  timeline, deprecation window, and FFmpeg-patches coordination plan.

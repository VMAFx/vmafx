## Added

- **Doxygen comments on under-documented public libvmaf C-API entry points
  (round 3)** — round-3 follow-on to PR #302 (round 1) and PR #327 (round 2).
  Adds `@brief`, `@param`, `@field`, ownership/lifetime contracts, and
  `@since` annotations to 12 public surfaces across `picture.h`,
  `libvmaf.h`, and `libvmaf_cuda.h` that downstream consumers
  (`ffmpeg-patches/`, the Go/Rust bindings, the MCP server, the tiny-AI
  ONNX harness) depend on:

  - `picture.h`: `VmafPixelFormat` (enum), `VmafRef` (opaque typedef),
    `vmaf_picture_alloc`, `vmaf_picture_unref`.
  - `libvmaf.h`: `VmafLogLevel` (enum), `VmafOutputFormat` (enum),
    `VmafContext` (opaque typedef), `VmafPictureConfiguration` (struct).
  - `libvmaf_cuda.h`: `VmafCudaState` (opaque typedef),
    `VmafCudaConfiguration` (struct), `VmafCudaPicturePreallocationMethod`
    (enum), `VmafCudaPictureConfiguration` (struct).

  The blocks document refcount semantics (`vmaf_picture_unref` returns the
  picture to its pool when preallocated, frees buffers otherwise),
  ownership-transfer rules (handing a `VmafPicture` to
  `vmaf_read_pictures` transfers ownership), the planar pixel-format
  conventions (subsampling, the `YUV400P` luma-only special case), and the
  CUDA preallocation storage tiers (`DEVICE` / `HOST` / `HOST_PINNED`).

- **NOLINT citations on upstream-mirror include guards**: the three touched
  Netflix-copyright headers retain their `__VMAF_*_H__` include guards
  verbatim for rebase parity with Netflix/vmaf master. Each `#ifndef` /
  `#define` carries an inline NOLINT for `bugprone-reserved-identifier`
  citing CLAUDE.md §10 (Upstream sync) and §12 r12 (touched-file
  lint-clean rule per ADR-0278). Same pattern PR #327 used for
  `feature.h` / `model.h` / `dnn.h`. No identifier changes; no ABI impact.

No semantic or ABI change. Doc-only comment additions plus the NOLINT
annotations required to leave the touched files lint-clean.

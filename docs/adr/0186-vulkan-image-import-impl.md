# ADR-0186: Vulkan VkImage import + filter (T7-29 parts 2 + 3)

- **Status**: Accepted
- **Date**: 2026-04-27
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: vulkan, ffmpeg, fork-local, zero-copy, implementation

## Context

[ADR-0184](0184-vulkan-image-import-scaffold.md) shipped the
public surface for the Vulkan VkImage import path as
`-ENOSYS`-returning stubs:

- `vmaf_vulkan_import_image()`
- `vmaf_vulkan_wait_compute()`
- `vmaf_vulkan_read_imported_pictures()`

The signatures landed (PR #128) so downstream consumers — the
future `libvmaf_vulkan` FFmpeg filter (T7-29 part 3) and any
direct C-API callers — could compile against the contract.
This ADR records the design decisions for the actual
implementation now that we are dropping the stubs.

The implementation is needed before T7-29 part 3 (the FFmpeg
filter) can land; the filter code is otherwise untestable.

## Decision

Implement the three import entry points with a **synchronous v1
design** plus a documented v2 follow-up. Add a fourth entry
point — `vmaf_vulkan_state_init_external` — so the FFmpeg
filter can run libvmaf compute on the decoder's VkDevice
(source VkImage handles are device-bound; cross-device import
would require dmabuf export/import plumbing that is out of
scope for v1). Bundle the FFmpeg filter
(`ffmpeg-patches/0006-libvmaf-add-libvmaf-vulkan-filter.patch`)
in the same PR — see "FFmpeg patch coupling" below.

### Per-state staging buffers

`VmafVulkanState` gains a `struct VmafVulkanImportSlots` field
holding **one ref + one dis staging `VkBuffer`** (HOST_VISIBLE
| HOST_COHERENT, allocated via VMA), reused across frames. The
buffers are sized to match the `DATA_ALIGN`-rounded stride that
`vmaf_picture_alloc` would produce — so the pixel data can be
handed straight to `vmaf_read_pictures` without an additional
host memcpy on the libvmaf side.

Geometry (w, h, bpc) is pinned by the **first**
`vmaf_vulkan_import_image()` call. Subsequent calls must match
or return `-EINVAL` — same contract as the SYCL
`vmaf_sycl_init_frame_buffers()` model. Lazy allocation avoids
needing a separate init entry point in the public surface.

### Synchronous copy path (v1)

Inside `vmaf_vulkan_import_image()` we:

1. Lazy-allocate the staging buffers + a reusable command
   buffer + a fence on first call.
2. Record the command buffer:
   - `vkCmdPipelineBarrier`: caller's `vk_layout` →
     `TRANSFER_SRC_OPTIMAL` (we do not transition back —
     AVVkFrame discardable semantics).
   - `vkCmdCopyImageToBuffer` for the Y plane only.
3. Submit with the caller's timeline semaphore as
   `pWaitSemaphores` (or skip the wait when
   `vk_semaphore == 0`, e.g. for the smoke test).
4. **Wait the fence in-call** before returning.

`vmaf_vulkan_wait_compute()` is therefore a **no-op** on this
path — the work has already drained. The function is kept in
the surface so the v2 async-pending-fence model can drop in
without an ABI change.

`vmaf_vulkan_read_imported_pictures(ctx, index)` (in
`libvmaf.c` under `HAVE_VULKAN`) wraps the staging buffers'
host pointers into proper `VmafPicture` handles via a builder
in `import.c`, attaches a no-op release callback (the buffers
are owned by the state, not the picture pool), and routes
through the standard `vmaf_read_pictures()` pipeline.

### Why YUV400P (luma-only)

The first iteration emits luma-only `VmafPicture` (`pix_fmt =
VMAF_PIX_FMT_YUV400P`). Every fork-added Vulkan extractor
shipped to date — psnr, vif, motion, adm, moment — is luma-only,
so chroma planes are never read. Adding chroma support is a
mechanical extension when the first chroma-aware extractor
arrives.

### File split

- `core/src/vulkan/import.c` (new, ~310 LOC): the buffer
  lifecycle, command-buffer recording, fence wait, and the
  `vmaf_vulkan_state_build_pictures()` builder.
- `core/src/vulkan/import_picture.h` (new): exposes the
  builder so `libvmaf.c` can include it without inheriting
  `<volk.h>`.
- `core/src/vulkan/vulkan_internal.h`: gains
  `VmafVulkanImportSlots`, `owns_handles`, and promotes
  `VmafVulkanState` from `common.c` so both files can see the
  slot layout.
- `core/src/vulkan/common.c`: adds
  `vmaf_vulkan_state_init_external` + the matching internal
  `vmaf_vulkan_context_new_external` that adopts caller-
  supplied handles, skipping `vkCreate{Instance,Device}`.
- `core/src/libvmaf.c`: implements
  `vmaf_vulkan_read_imported_pictures()` next to the existing
  `vmaf_vulkan_import_state()`.
- `ffmpeg-patches/0006-libvmaf-add-libvmaf-vulkan-filter.patch`
  (new, ~280 LOC of additions to FFmpeg n8.1): the
  `libvmaf_vulkan` filter consuming `AV_PIX_FMT_VULKAN`,
  pulling `AVVkFrame *` from `data[0]`, calling
  `vmaf_vulkan_state_init_external` with the device's compute
  queue, then `import_image` + `read_imported_pictures` per
  frame. Mirrors `0005-libvmaf-add-libvmaf-sycl-filter.patch`.

### FFmpeg patch coupling (new fork rule)

Bundling parts 2 + 3 surfaces a recurring failure mode: the
fork ships its FFmpeg integration as a stack of patches against
`n8.1`, and any libvmaf-side surface change probed by those
patches breaks the next rebase silently. This PR adds rule §12
r14 to [`CLAUDE.md`](../../CLAUDE.md) (and the AGENTS.md
mirror): every PR that touches a libvmaf public surface used by
`ffmpeg-patches/` updates the relevant patch in the **same PR**
— pure libvmaf-internal refactors, doc-only, and test-only PRs
are exempt. Reviewers verify with
`for p in ffmpeg-patches/000*-*.patch; do git -C ffmpeg-8 apply
--check "$p"; done` against the pinned `n8.1` baseline.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Async pending-fence v2** — record + submit in `import_image`, return immediately, track the fence; `wait_compute` blocks on the fence | True overlap of decode/copy; lower latency on the fast path | Requires a per-frame fence pool, dual-buffering for outstanding submits, careful interaction with state lifecycle; doubles the test matrix | v1 is enough to unlock T7-29 part 3 (FFmpeg filter) which already serializes per-frame; v2 is a follow-up once profiling shows the wait is a bottleneck |
| **Kernels read VkImage directly** via `VkSampler` / storage-image bindings | True zero-copy on the GPU side | Requires refactoring every Vulkan extractor (psnr/vif/motion/adm/moment) to support both `VmafVulkanBuffer` and `VkImage` inputs; ~3-5x the LOC of v1 | Out of scope for T7-29 part 2; revisit after part 3 ships and the FFmpeg-side workflow is real |
| **Build a fake `VmafPicture` without a `VmafRef`** | Avoids the ref-init / no-op-release-callback dance | `vmaf_read_pictures()` always calls `vmaf_picture_unref` on cleanup, which returns `-EINVAL` on `pic->ref == NULL` and propagates that as a non-zero return from `vmaf_read_pictures` | Following the existing release-callback contract is cleaner; the per-frame overhead is a single `vmaf_ref_init` + decrement |
| **Allocate via `vmaf_picture_alloc` + memcpy from staging into the alloc'd picture** | No release-callback wiring | Adds one host memcpy per plane per frame on top of the existing extractor `upload_plane` memcpy | The release-callback approach is only ~25 LOC and avoids the extra memcpy |
| **Defer to T7-29 part 3** — implement everything inside the FFmpeg filter without a libvmaf-side surface | Smaller libvmaf footprint | The filter ends up reaching into Vulkan handle internals to do `vkCmdCopyImageToBuffer` against an internal `VmafVulkanBuffer` — leaks abstraction and duplicates logic for any direct C-API caller | The C-API surface is the contract; the FFmpeg filter is one consumer |

## Consequences

- **Positive**: All three import entry points return `0` on the
  success path. Geometry validation matches SYCL's `init_frame_buffers`
  contract. The staging-buffer reuse (one allocation per state,
  not per frame) keeps the per-frame cost to one
  `vkCmdCopyImageToBuffer` + one fence wait. Header purity from
  ADR-0184 is preserved (no `<volk.h>` leaks into `libvmaf.c`).
- **Negative**: v1 is synchronous — every
  `vmaf_vulkan_import_image()` call blocks the caller until the
  GPU finishes the copy. For a 1080p 8-bit Y plane this is
  sub-millisecond (~2 MB at >5 GB/s PCIe), but it precludes
  decode/copy overlap. Documented; v2 follow-up addresses it.
- **Neutral / follow-ups**:
  1. **T7-29 part 3 (S)** — package the FFmpeg-side
     `libvmaf_vulkan` filter as
     `ffmpeg-patches/0006-libvmaf-add-libvmaf-vulkan-filter.patch`.
     Now possible because the API works.
  2. **v2 async pending-fence model (deferred)** — once
     part 3 ships and is exercised, profile to confirm the
     synchronous wait is the bottleneck before refactoring.
  3. **Chroma support (deferred)** — extend the staging-buffer
     pair to ref/dis × Y/U/V (or a single plane-stride array)
     when the first chroma-aware Vulkan extractor lands.
  4. **Validation layer integration (deferred)** —
     `VmafVulkanConfiguration.enable_validation` is still a
     no-op; the field reservation lands in T5-1c.

## Verification

End-to-end GPU-plumbing validation lives downstream in T7-29
part 3 (the FFmpeg filter): the natural test is
`ffmpeg -hwaccel vulkan ... -vf libvmaf_vulkan` and verifying
the score matches the CPU-path baseline at `places=4`. For
this PR, validation is **contract-level**:

- 10/10 unit tests in
  [`core/test/test_vulkan_smoke.c`](../../core/test/test_vulkan_smoke.c)
  cover: NULL-state rejection, `vk_image == 0` rejection,
  `wait_compute` on an idle state returns 0,
  `read_imported_pictures` on a NULL ctx → -EINVAL.
- The float_moment Vulkan cross-backend gate
  (`scripts/ci/cross_backend_vif_diff.py --feature float_moment
  --backend vulkan`) re-runs clean: 0/48 mismatches × 4 metrics
  on Intel Arc A380 — confirms the import-slot promotion and
  state struct change did not regress the existing kernel
  paths.

## References

- Parent: [ADR-0184](0184-vulkan-image-import-scaffold.md)
  — declares the API shape this ADR implements.
- Pattern source: SYCL trio
  (`vmaf_sycl_import_va_surface` /
  `vmaf_sycl_wait_compute` / `vmaf_read_pictures_sycl`)
  in [`libvmaf_sycl.h`](../../core/include/libvmaf/libvmaf_sycl.h).
- Source: T7-29 in
  [`.workingdir2/BACKLOG.md`](../../.workingdir2/BACKLOG.md).
- Per-PR rule: ADR-0108 deep-dive deliverables checklist.

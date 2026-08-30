# ADR-0184: Vulkan VkImage import C-API scaffold (T7-29 part 1 of 2)

- **Status**: Accepted
- **Date**: 2026-04-26
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: vulkan, ffmpeg, fork-local, zero-copy, scaffold

## Context

PR #126 surfaced a real ergonomic gap: when FFmpeg users decode
video via `-hwaccel vulkan -hwaccel_output_format vulkan`, the
regular `libvmaf` filter forces a `hwdownload,format=yuv420p`
round-trip. PR #127 (T7-28) closed the symmetric SYCL gap by
packaging an existing local `libvmaf_sycl` filter that consumed
oneVPL frames via the **already-existing**
`vmaf_sycl_import_va_surface` C-API.

T7-29 has no such pre-existing C-API. FFmpeg's
`AV_PIX_FMT_VULKAN` carries a stack of `VkImage` handles in an
`AVVkFrame` (one per plane) plus timeline semaphores
(`VkSemaphore` + `uint64_t` wait/signal value). To consume those
zero-copy, libvmaf needs a new public surface in
[`libvmaf_vulkan.h`](../../core/include/libvmaf/libvmaf_vulkan.h)
that accepts external `VkImage` + `VkSemaphore` and either
exposes them to the existing compute kernels or copies them
into the internal `VmafVulkanBuffer` shape that the kernels
already consume.

That's a multi-day engineering pass. Following the ADR-0175
precedent (Vulkan backend originally landed as a scaffold-only
surface with `-ENOSYS` stubs), this ADR ships the C-API
**declarations only** so downstream consumers can compile
against the surface; the real implementation is a focused
follow-up PR (T7-29 part 2).

## Decision

Add three new entry points to
[`libvmaf_vulkan.h`](../../core/include/libvmaf/libvmaf_vulkan.h),
all returning `-ENOSYS` in this scaffold PR:

```c
/* Import an external VkImage into the libvmaf Vulkan compute
 * pipeline. The state holds onto the image until the next
 * vmaf_vulkan_wait_compute() returns. Caller retains ownership
 * of the underlying VkImage and VkSemaphore.
 *
 *   vk_image          : VkImage handle (cast to uintptr_t for
 *                       header purity — avoids leaking
 *                       <vulkan/vulkan.h> from libvmaf_vulkan.h).
 *   vk_format         : VkFormat enum value (uint32_t).
 *   vk_layout         : current VkImageLayout enum value.
 *   vk_semaphore      : timeline semaphore handle.
 *   vk_semaphore_value: wait value (libvmaf will wait until the
 *                       semaphore reaches this value before reading).
 *   w, h, bpc         : frame geometry.
 *   is_ref            : 1 = reference frame, 0 = distorted.
 *   index             : frame index (matches vmaf_read_pictures()). */
int vmaf_vulkan_import_image(VmafVulkanState *state,
                             uintptr_t vk_image,
                             uint32_t vk_format,
                             uint32_t vk_layout,
                             uintptr_t vk_semaphore,
                             uint64_t vk_semaphore_value,
                             unsigned w, unsigned h, unsigned bpc,
                             int is_ref, unsigned index);

/* Block until all previously-submitted compute work on `state`
 * has finished. Used by FFmpeg-side filters before reusing
 * imported images in the next frame. */
int vmaf_vulkan_wait_compute(VmafVulkanState *state);

/* Trigger a libvmaf score read for the imported reference +
 * distorted images at `index`. Mirrors vmaf_read_pictures_sycl
 * but for Vulkan-imported frames. */
int vmaf_vulkan_read_imported_pictures(VmafContext *ctx,
                                       unsigned index);
```

**Why three entry points** (mirrors the SYCL surface):
`vmaf_sycl_import_va_surface` + `vmaf_sycl_wait_compute` +
`vmaf_read_pictures_sycl` is the established trio; the FFmpeg
filter uses all three. Symmetric API shape lets the future
`libvmaf_vulkan` filter follow the same pattern as PR #127's
`libvmaf_sycl` filter byte-for-byte modulo names.

**Header purity** — the public header takes `uintptr_t` /
`uint32_t` for Vulkan handles instead of including
`<vulkan/vulkan.h>`. Same pattern as the existing
`libvmaf_cuda.h` (uses `void *` for `CUcontext`). Keeps the
surface usable from translation units that don't have Vulkan
headers in scope.

**Stub returns**: every function returns `-ENOSYS` in this PR.
Same pattern as ADR-0175 used for the original Vulkan scaffold.
The FFmpeg-side `libvmaf_vulkan` filter (T7-29 part 2) is **not**
in this PR — it lands together with the real implementation,
because its call path needs the implementation to exist.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Full implementation in one PR (kernels consume VkImages directly + new FFmpeg filter + tests) | One coherent shipping unit | 1000+ LOC; touches kernel pipelines, changes the established `VmafVulkanBuffer` API, blocks on getting all of it right at once | The Vulkan backend itself shipped scaffold-first via ADR-0175; same precedent applies |
| Full impl with internal `vkCmdCopyImageToBuffer` (kernels stay on `VmafVulkanBuffer`) | Smaller than refactoring kernels; "almost zero-copy" — image → buffer copy stays on GPU but isn't strictly zero-copy | Still ~600-800 LOC; commits us to a specific copy strategy that may not be optimal | Same logic — follow-up PR can pick the best strategy with profiling data |
| Just declare the API + stub impl + ADR (chosen) | Unblocks the FFmpeg-side filter writing without committing to an internal copy strategy; matches ADR-0175 precedent; small PR | The API surface lands but does nothing useful yet; users still hit `-ENOSYS` if they call it | Best foundation for a focused follow-up; shipping nothing useful is fine when the surface lands as a stable contract |
| Defer T7-29 entirely (`hwdownload` bridge stays the only Vulkan path forever) | Less work | Symmetric ergonomic gap with SYCL persists; FFmpeg users get worse Vulkan UX than SYCL UX | Asymmetry is bad UX; even a scaffolded API closes the documentation gap |

## Consequences

- **Positive**: API surface lands as a stable contract.
  Downstream code (incl. the future `libvmaf_vulkan` FFmpeg
  filter and any direct C-API callers) can compile against it
  today. The header-purity choice (`uintptr_t` for handles)
  matches existing precedent.
- **Negative**: stub returns `-ENOSYS` until T7-29 part 2
  lands; users calling the new entry points get an immediate
  failure. Same pattern users already saw on the original
  Vulkan scaffold — predictable, documented, time-boxed by the
  follow-up commit.
- **Neutral / follow-ups**:
  1. **T7-29 part 2 (M-L)** — implement the three entry points.
     Needs internal `VkBuffer` allocation + `vkCmdCopyImageToBuffer`
     with proper layout transition + timeline-semaphore wait.
  2. **T7-29 part 3 (S)** — package the FFmpeg-side
     `libvmaf_vulkan` filter as
     `ffmpeg-patches/0006-libvmaf-add-libvmaf-vulkan-filter.patch`,
     mirroring PR #127's `0005-*.patch` for SYCL.
  3. **Future optimisation (deferred)** — kernels reading
     `VkImage` directly via `VkSampler` / storage-image bindings,
     skipping the internal copy. Bigger refactor; only worth it
     if profiling shows the copy is the bottleneck.

## References

- Source: T7-29 in
  [`.workingdir2/BACKLOG.md`](../../.workingdir2/BACKLOG.md);
  exposed as the symmetric gap to T7-28 by PR #126 review.
- Pattern parent: [ADR-0175](0175-vulkan-backend-scaffold.md)
  (original Vulkan scaffold-first decision); [ADR-0183](0183-ffmpeg-libvmaf-sycl-filter.md)
  (T7-28 SYCL filter — the symmetric surface this T7-29 work
  closes against).
- C-API surface to mirror:
  [`libvmaf_sycl.h`](../../core/include/libvmaf/libvmaf_sycl.h) —
  `vmaf_sycl_import_va_surface`,
  `vmaf_sycl_wait_compute`,
  `vmaf_read_pictures_sycl`.
- FFmpeg-side data shape:
  [`/usr/include/libavutil/hwcontext_vulkan.h`](https://ffmpeg.org/doxygen/trunk/hwcontext__vulkan_8h.html)
  — `AVVkFrame.img[]`, `AVVkFrame.layout[]`,
  `AVVkFrame.sem[]`, `AVVkFrame.sem_value[]`.

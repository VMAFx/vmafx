# Vulkan image-import C API

> **Status: REMOVED — [ADR-0726](../adr/0726-drop-vulkan-backend.md) (2026-05-28)**
>
> The Vulkan backend and its image-import API were removed from the fork in
> ADR-0726. `core/include/libvmaf/libvmaf_vulkan.h` no longer exists.
> The `vf_libvmaf_vulkan` FFmpeg filter was replaced with a no-op shim per
> [ADR-0860](../adr/0860-ffmpeg-patch-chain-no-op-vulkan-shim.md).
> For zero-copy frame import, see the SYCL D3D11/dmabuf import path in
> [`docs/development/windows-d3d11-import.md`](../development/windows-d3d11-import.md).
>
> **This page is historical documentation** describing the Vulkan image-import
> design that was implemented before removal.

---

The Vulkan backend exposed a zero-copy import surface so callers
that already had decoded frames sitting in a `VkImage` could score
them in libvmaf without round-tripping through the host. This was
the Vulkan analogue of the SYCL D3D11 / dmabuf import path
documented in
[`docs/development/windows-d3d11-import.md`](../development/windows-d3d11-import.md);
the design background lives in
[ADR-0184](../adr/0184-vulkan-image-import-scaffold.md) (scaffold)
and [ADR-0186](../adr/0186-vulkan-image-import-impl.md) (impl +
ffmpeg-patch wiring).

The public header was
`core/include/libvmaf/libvmaf_vulkan.h` (now removed).
The Vulkan backend historical overview lives in
[`docs/backends/vulkan/overview.md`](../backends/vulkan/overview.md).

## Use cases

- An FFmpeg filter (`vf_libvmaf_vulkan` per
  [ADR-0186](../adr/0186-vulkan-image-import-impl.md)) receives
  AVFrame instances backed by `AVHWDeviceContext` of type
  `AV_HWDEVICE_TYPE_VULKAN`. Importing the underlying `VkImage`
  directly avoids the H2D + D2H round-trip the CPU score path
  would incur.
- A custom Vulkan renderer wants to score its own output against a
  reference, both already in device memory. Importing both frames
  keeps the entire pipeline on-device.

## Lifecycle

```c
#include <libvmaf/libvmaf.h>
#include <libvmaf/libvmaf_vulkan.h>

VmafContext         *ctx     = /* ... vmaf_init() ... */;
VmafVulkanState     *state   = NULL;
VmafVulkanConfiguration cfg  = { .device_index = -1, .enable_validation = 0,
                                 .max_outstanding_frames = 0 };

int rc = vmaf_vulkan_state_init(&state, cfg);
/* OR pass an existing VkInstance/VkDevice/queue from your renderer:
 *   vmaf_vulkan_state_init_external(&state, &handles, cfg);
 * — keeps libvmaf compute on the SAME VkDevice as your source VkImages.
 */

for (unsigned i = 0; i < n_frames; i++) {
    rc = vmaf_vulkan_import_image(state,
                                  (uintptr_t) ref_image_i,
                                  ref_format,         /* VkFormat */
                                  ref_layout,         /* VkImageLayout */
                                  (uintptr_t) ref_semaphore,
                                  ref_semaphore_value,
                                  width, height, bpc,
                                  /* is_ref */ 1, i);
    rc = vmaf_vulkan_import_image(state,
                                  (uintptr_t) dist_image_i,
                                  dist_format, dist_layout,
                                  (uintptr_t) dist_semaphore,
                                  dist_semaphore_value,
                                  width, height, bpc,
                                  /* is_ref */ 0, i);
    rc = vmaf_vulkan_read_imported_pictures(ctx, i);
}

rc = vmaf_vulkan_wait_compute(state);
/* ... score readout via vmaf_score_at_index / vmaf_score_pooled() ... */
```

## Entry-point reference

| Symbol                                 | Purpose                                                                                                                                                                                                                                                       |
|----------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `vmaf_vulkan_available()`              | Build-time probe — returns 1 if libvmaf was built with `-Denable_vulkan=enabled`. Safe to call before any Vulkan runtime is loaded.                                                                                                                           |
| `vmaf_vulkan_state_init()`             | Allocate a state with libvmaf-owned `VkInstance` + `VkDevice` + compute queue. `device_index = -1` picks the first compute-capable device.                                                                                                                    |
| `vmaf_vulkan_state_init_external()`    | Allocate a state that reuses a caller-supplied `VkInstance` / `VkDevice` / queue. Required when the import target lives on a specific device.                                                                                                                 |
| `vmaf_vulkan_import_image()`           | Bind one `VkImage` (cast to `uintptr_t`) to a frame index + ref/dist slot. Caller retains ownership; libvmaf reads only. Honours the `VkSemaphore` + value the caller passed (libvmaf waits until the semaphore reaches `vk_semaphore_value` before reading). |
| `vmaf_vulkan_read_imported_pictures()` | Trigger the score read for the imported `(ref, dist)` pair at `index`. Mirrors `vmaf_read_pictures_sycl()` for Vulkan-imported frames.                                                                                                                        |
| `vmaf_vulkan_wait_compute()`           | Block until all submitted compute work has finished. Required before reusing imported `VkImage` slots in the next frame.                                                                                                                                      |
| `vmaf_vulkan_state_close()`            | Tear down. Caller-owned `VkImage` / `VkSemaphore` handles are *not* destroyed.                                                                                                                                                                                |

The full Doxygen-style API contract — including `-EINVAL` /
`-ENOSYS` error codes and threading rules — lives in the header.

## Image format support

| Component          | Accepted values                                                                                                                                                                                                                                           |
|--------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `VkFormat` (luma)  | `VK_FORMAT_R8_UNORM` (8-bpc), `VK_FORMAT_R16_UNORM` (10/12/16-bpc).                                                                                                                                                                                       |
| `VkImageLayout`    | Any layout legal as a transfer-source / shader-read source for the requested format. libvmaf inserts the necessary barriers internally.                                                                                                                   |
| Bits per component | 8, 10, 12, 16.                                                                                                                                                                                                                                            |
| Width × height     | Any size the underlying compute backend accepts. Multi-plane (NV12 / YUV420) frames are passed as separate `VkImage` per plane via repeated `import_image` calls (see `is_ref` / index sequencing in the FFmpeg-filter source for the canonical pattern). |

## Synchronisation

libvmaf does not own the source images. Each
`vmaf_vulkan_import_image` call carries a `VkSemaphore` + a 64-bit
wait value; libvmaf binds a wait operation on its compute queue
that blocks until `semaphore.value >= vk_semaphore_value`. Callers
must signal the semaphore from whatever queue produced the image
before, or independent of, the import. Use a *timeline* semaphore
(promoted to core in Vulkan 1.2; required by the import contract)
so the value can advance monotonically across multiple producers.

After the readout loop, `vmaf_vulkan_wait_compute()` blocks until
all libvmaf-internal compute submissions have drained — call this
before reusing the source `VkImage` for the next frame.

## Build / runtime

The import path is gated on the Vulkan backend being built in:

```bash
meson setup build -Denable_vulkan=enabled
ninja -C build
```

Runtime detection: `vmaf_vulkan_available()` returns 1 only if the
build flag was set; without it every entry point returns
`-ENOSYS`. Validation-layer support (`enable_validation = 1` in
`VmafVulkanConfiguration`) requires `VK_LAYER_KHRONOS_validation`
on the host.

## See also

- [`docs/backends/vulkan/overview.md`](../backends/vulkan/overview.md)
  — backend overview, kernel matrix, build flags.
- [`core/include/libvmaf/libvmaf_vulkan.h`](../../core/include/libvmaf/libvmaf_vulkan.h)
  — authoritative API surface.
- [`docs/usage/ffmpeg.md`](../usage/ffmpeg.md) — `vf_libvmaf_vulkan`
  filter that consumes the import API end-to-end.
- [ADR-0184](../adr/0184-vulkan-image-import-scaffold.md) /
  [ADR-0186](../adr/0186-vulkan-image-import-impl.md) — design
  decisions.
- [Research-0086](../research/0086-usage-doc-coverage-audit-2026-05-08.md)
  — audit that triggered this page.

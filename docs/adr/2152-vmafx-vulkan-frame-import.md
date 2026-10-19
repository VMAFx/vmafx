<!-- markdownlint-disable MD013 MD060 -->
# ADR-2152: VMAFx imports Vulkan frames on CUDA, SYCL and HIP: one memory kind and one semaphore fence kind, opaque memory on CUDA and the dma-buf route elsewhere

- **Status**: Accepted (2026-10-07, Q-089)
- **Date**: 2026-10-06
- **Deciders**: maintainer (popup 2026-10-07, Q-089); RC4 work package 3 (Vulkan import lane); maintainer decision Q-011 (import only)
- **Tags**: api, rc4, gpu, cuda, sycl, hip, vulkan, zero-copy

## Context

FFmpeg's Vulkan decode, libplacebo and GStreamer's Vulkan decoders produce
frames in Vulkan memory. Before this change none of them reached VMAFx
without a round trip through the host. The maintainer decided (Q-011) that
Vulkan is import only: VMAFx gets no Vulkan compute and no Vulkan backend in
1.0, so the library never creates a Vulkan object; it imports what a Vulkan
producer exported into the backends that score, CUDA, SYCL and HIP. The
shared import contract is [ADR-1929](1929-vmafx-device-frames-fences.md);
the backend lanes are [ADR-2023](2023-vmafx-cuda-device-frames.md) (CUDA),
[ADR-2091](2091-vmafx-sycl-device-frames.md) (SYCL) and
[ADR-2092](2092-vmafx-hip-device-frames.md) (HIP). API changes are additive
([ADR-1897](1897-vmafx-abi-0x-numbering.md)).

The three runtimes take Vulkan exports differently, measured on the pinned
toolchains ([Research-2162](../research/2162-vmafx-vulkan-frame-import.md)):

- CUDA imports opaque-fd memory as buffers or mipmapped arrays (OPTIMAL
  images read correctly), and opaque-fd timeline semaphores it waits on and
  signals on a stream. CUDA 13.4 imports dma-bufs on Tegra only.
- On the Mesa drivers (ANV, RADV) an opaque memory descriptor is a dma-buf,
  so the SYCL and HIP lanes' dma-buf paths read LINEAR images and buffers,
  and the SYCL lane reads Tile4 DRM-modifier images. Neither reads a
  driver-private OPTIMAL layout reliably: Level Zero has no such import, and
  HIP read an OPTIMAL P010 chroma plane back wrong in one of five probe runs.
- ROCm 10.1 imports no Vulkan semaphore (timeline: `hipErrorInvalidValue`;
  opaque fd and sync_fd: `hipErrorNotSupported`). Level Zero imports a
  Vulkan timeline semaphore, but the SYCL lane has no device-side wait on one
  yet. Both lanes wait on a sync_file of the producer's write on the host.
- FFmpeg's Vulkan decoder outputs NV12 as one two-plane OPTIMAL image whose
  memory is exportable on the NVIDIA driver only; a frame with one exportable
  image per plane needs a device copy on the producer's side.

## Decision

1. **One memory kind, one fence kind.** `VMAFX_MEMORY_VULKAN` (9) describes
   each plane by the exported memory object (`fd`, or `handle` on Windows),
   its allocation `size`, the plane's `offset` (image bind offset plus plane
   offset), `pitch` and `modifier`, with `VmafxFrameImport.vulkan_handle_type`
   (the Vulkan handle type bit), `vulkan_tiling` (the `VkImageTiling` value),
   `vulkan_flags` (`VMAFX_VULKAN_DEDICATED`) and `vulkan_pci` (the producer's
   PCI location, `VK_EXT_pci_bus_info`). `VMAFX_FENCE_VULKAN_SEMAPHORE` (9)
   is a timeline semaphore exported as an opaque descriptor and a value.
   `VmafxFrameImport.acquire_more[2]` holds the further acquire fences of a
   frame written under several timelines (one image per plane). Images are in
   `VK_IMAGE_LAYOUT_GENERAL` and released to `VK_QUEUE_FAMILY_EXTERNAL`.
   `VmafxDeviceInfo.pci` gives each device's PCI location so a producer can
   pick the same GPU. ABI 0.1.5.
2. **The library never touches Vulkan.** No Vulkan loader is linked and no
   Vulkan call is made. `vmafx_fence_wait()` and `vmafx_fence_destroy()`
   refuse a `VULKAN_SEMAPHORE` fence by name.
3. **CUDA imports the opaque memory itself** (`core/src/cuda/import_vulkan.c`):
   OPTIMAL images as mipmapped arrays (8 or 16 bit, 2 channels for
   interleaved chroma; a planar OPTIMAL frame needs `VMAFX_IMPORT_ALLOW_COPY`,
   as CUDA arrays do), LINEAR images and buffers as device pointers. The
   acquire timelines are waited on the library stream; the producer's
   timeline is signalled behind the frame's last reader in every context
   through the new `vmafx_frame_signal_on_release()`. DMA_BUF memory and
   DRM-modifier tiling are refused naming the field.
4. **SYCL and HIP take the dma-buf route**: a LINEAR or DRM-modifier frame
   whose memory is a dma-buf becomes the `VMAFX_MEMORY_DMABUF` descriptor of
   ADR-2091 / ADR-2092 (`vmafx_import_vulkan_as_dmabuf()`), with the lanes'
   modifier rules (SYCL: linear, Tile4, Y-tiled; HIP: linear). OPTIMAL tiling,
   a Vulkan semaphore acquire and further acquire fences are refused by name;
   the acquire is a sync_file (`SYNC_FD` export of the producer's write) or a
   host fence, and the release is the lanes' HOST release fence, after which
   the producer signals its own timeline.
5. **Refused by name, never read across devices or through the host**:
   memory of another GPU (`desc.vulkan_pci`, or `device` when the backend
   reports no PCI location), a plane of a multi-plane image
   (`desc.plane[i].plane_index`: export one image per plane), Windows handles
   (`desc.vulkan_handle_type`, until a Windows device runs the tests), an
   unknown handle type or tiling, a LINEAR plane with a modifier.
6. **SYNC_FILE fences are destroyed by closing them**, as ADR-2091 decided:
   the merge of the SYCL and HIP lanes in this branch keeps the SYCL lane's
   behaviour, and `test_vmafx_fence_kinds` checks that the descriptor is
   closed.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| A memory kind and a semaphore fence kind with lane-specific routes (chosen) | One descriptor maps `AVVkFrame` and `GstVulkanImageMemory`; each lane takes what its runtime measured to read | SYCL and HIP wait for the producer on the host | — |
| A Vulkan device inside the library to copy or convert frames | Any tiling on any backend; device-side semaphore waits everywhere | Contradicts Q-011 (import only); a Vulkan dependency in every build | Maintainer decision |
| Express Vulkan frames as the existing `DMABUF` kind only | No new kind | CUDA reads no dma-buf (Tegra only), OPTIMAL images and timeline semaphores have no place | CUDA, the main Vulkan producer's partner, would be excluded |
| Read OPTIMAL images on HIP through mipmapped arrays | Covers FFmpeg's default tiling on AMD | One size measured, one wrong read in five runs, no evidence for compressed layouts | Refused by name until measured (Research-2162, Open) |
| Wait on the Vulkan timeline on the SYCL device through Level Zero | No host wait | The SYCL queue has no wait on an imported semaphore yet; new interop path in the RC4 timeframe | Follow-up; the sync_file acquire works today |
| Accept FFmpeg's two-plane decoder image | No producer-side copy | CUDA faulted on an array over the second plane; memory not exportable on RADV / ANV | Refused naming `plane_index`; the producer copies into per-plane images |

## Consequences

- **Positive**: Vulkan frames score on CUDA, SYCL and HIP with no host copy
  and bit for bit as host uploads; on CUDA the producer's own timeline orders
  both the write and the reuse on the device.
- **Negative**: on SYCL and HIP the acquire and the release are host waits;
  FFmpeg's decoder output needs a device copy into one image per plane;
  OPTIMAL images score on CUDA only.
- **Neutral / follow-ups**: a device-side Vulkan semaphore wait on SYCL
  through Level Zero; OPTIMAL images on HIP once measured at every size;
  Windows handles once a Windows device runs the tests; the WP9 FFmpeg filter
  and GStreamer element map their frames onto the descriptor (request
  `WP3-vulkan-1`); per-import external-memory and semaphore imports on CUDA
  are an RC7 tuning row (a cache keyed by the exported object).

## References

- [ADR-1829](1829-rc4-zero-copy-import.md), [ADR-1852](1852-vmafx-api-redesign.md) design section 2.7, [ADR-1897](1897-vmafx-abi-0x-numbering.md), [ADR-1929](1929-vmafx-device-frames-fences.md), [ADR-2023](2023-vmafx-cuda-device-frames.md), [ADR-2091](2091-vmafx-sycl-device-frames.md), [ADR-2092](2092-vmafx-hip-device-frames.md), [Research-2162](../research/2162-vmafx-vulkan-frame-import.md).
- `req` (RC4 work package index, #1723 comment 2026-10-06): "Import FFmpeg Vulkan hardware frames (Vulkan Video decode) into CUDA / SYCL / HIP via Vulkan external memory + semaphores mapped to fence kinds (additive, ADR-1897); import only, no Vulkan compute (Q-011)".
- `req` (WP3 Vulkan import lane brief, 2026-10-06): "Design the descriptor so FFmpeg's `AVVkFrame` (per-plane or multi-plane images, `sem[]` / `sem_value[]`, layouts) and GStreamer's `GstVulkanImageMemory` both map onto it without copies."
- `req` (same brief): "Cross-vendor import (Vulkan on one GPU, scoring on another) is refused by name."
- `Q-011` (maintainer decision): "Vulkan is **import only** (no Vulkan compute, no native Vulkan backend in 1.0)."
- Tests: `core/test/test_vmafx_import_vulkan_api.c`, `test_vmafx_import_vulkan.c`, `test_vmafx_import_vulkan_bitexact.c`, `test_vmafx_import_vulkan_fence.c`, `test_vmafx_import_vulkan_ffmpeg.c` (each Vulkan test built per lane), `test_vmafx_fence_kinds.c`.
- `Q-089` (maintainer popup 2026-10-07): accepted as written.

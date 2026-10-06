<!-- markdownlint-disable MD013 MD060 -->
# Research-2162: What CUDA, Level Zero and HIP do with memory and semaphores a Vulkan producer exported

- **Status**: Active
- **Workstream**: [ADR-2152](../adr/2152-vmafx-vulkan-frame-import.md), [ADR-1829](../adr/1829-rc4-zero-copy-import.md), [ADR-2023](../adr/2023-vmafx-cuda-device-frames.md), [ADR-2091](../adr/2091-vmafx-sycl-device-frames.md), [ADR-2092](../adr/2092-vmafx-hip-device-frames.md)
- **Last updated**: 2026-10-06

## Question

FFmpeg's Vulkan decode, libplacebo and GStreamer's Vulkan decoders hand out
frames in Vulkan memory. VMAFx imports frames and never runs Vulkan itself
(maintainer decision Q-011: import only). Which of the producer's exports does
each scoring backend read without a host copy, which fences can it wait on and
signal, and where must the import refuse by name?

## Sources

- The test producer `core/test/vmafx_vulkan_producer.c`: exportable images
  (OPTIMAL, LINEAR, DRM-modifier) or buffers, one per plane, written by a
  staging copy on the producer's queue, a timeline semaphore per frame
  exported as an opaque descriptor, and on request a `SYNC_FD` export of a
  binary semaphore signalled with the write. The scratch probes that preceded
  the import code read every plane back through the scoring runtime and
  compared it with the bytes written.
- CUDA: the host's CUDA 13.4 (driver API 13040), NVIDIA driver 615.71.09,
  RTX 4090 at PCI 0000:06:00.0; Vulkan 1.4.351 on the device, loader 1.4.363.
- SYCL / Level Zero: oneAPI 2026.1.1 (DPC++ 2026.1.1.20260724) in a throwaway
  container of `vmaf-dev-mcp:local` with the Vulkan loader and headers added
  (headers 1.4.341); Level Zero GPU driver 26.35.39758.10, loader 1.34.0; ANV
  from Mesa 26.0.8 (the image's Mesa, not the host's 26.2.4), Vulkan 1.4.335;
  Arc A380 at PCI 0000:03:00.0 on the xe driver.
- HIP: ROCm 10.1.0 (HIP runtime 71626385) in `vmafx-hip-lane:rocm10.1.0`
  with the Vulkan loader and headers added; RADV from Mesa 26.0.8, Vulkan
  1.4.335, loader 1.4.341; the gfx1036 iGPU at PCI 0000:7d:00.0.
- FFmpeg n9.0.2 (libavutil 61.1.102): the host's build for the CUDA lane, and
  a build of the same tag inside the ROCm image for the SYCL and HIP lanes;
  Ubuntu's FFmpeg 8.0 packages were tried first.
- `cuda.h` of CUDA 13.4; `ze_api.h` of Level Zero 1.34; GStreamer 1.28
  `gstvkmemory.c` and the installed 1.28.7 headers.

## Findings

| Question | CUDA (RTX 4090) | Level Zero (A380) | HIP (gfx1036) |
| --- | --- | --- | --- |
| Opaque-fd memory of a buffer or LINEAR image | `cuImportExternalMemory()` + mapped buffer: bytes identical | The descriptor is a dma-buf (`/proc/self/fd` names `/dmabuf:`); imported through the SYCL lane's dma-buf path: identical | The descriptor is a dma-buf; imported through the HIP lane's dma-buf path: identical |
| Opaque-fd memory of an OPTIMAL image | Mipmapped array at the plane's bind offset, 1 channel (2 for interleaved chroma), 8 or 16 bit: identical for NV12, YUV420P and P010 | No image import of a driver-private layout; refused | `hipExternalMemoryGetMappedMipmappedArray()` read back identical in 4 of 5 probe runs at 576x324; one run returned a different P010 chroma plane. One size measured: refused |
| DMA_BUF memory with a DRM format modifier | `cuda.h` (13.4): dma-buf import "is supported only on Tegra platform starting with Thor series": refused | Linear and Tile4 (`0x0100000000000009`) imported and de-tiled by the SYCL lane (ADR-2091): identical | Linear imported; any other modifier refused (ADR-2092) |
| One image with two planes (FFmpeg's default NV12 frame) | A 2-channel array over the second plane of a multi-plane image faulted; refused naming `plane_index` | Refused | Refused |
| Timeline semaphore (opaque fd) as acquire | `cuImportExternalSemaphore()` TIMELINE_SEMAPHORE_FD + `cuWaitExternalSemaphoresAsync()` on the library stream | `zeDeviceImportExternalSemaphoreExt()` with `ZE_EXTERNAL_SEMAPHORE_EXT_FLAG_VK_TIMELINE_SEMAPHORE_FD` succeeds; not wired into the SYCL queue yet: refused | `hipImportExternalSemaphore()`: as TimelineSemaphoreFd `hipErrorInvalidValue`, as OpaqueFd `hipErrorNotSupported`: refused |
| Binary semaphore exported as SYNC_FD | Not needed | Polled on the host (ADR-2091 SYNC_FILE acquire) | Polled on the host; `hipImportExternalSemaphore()` of it as OpaqueFd: `hipErrorNotSupported` |
| Release back to the producer | `cuSignalExternalSemaphoresAsync()` of the producer's timeline behind the last reader (`vmafx_frame_signal_on_release()`) | HOST release fence, then the producer signals on the host | HOST release fence, then the producer signals on the host |
| Ordering of the import against the producer's write | Only through the acquire wait (a skipped wait reads unfinished frames) | Only through the acquire (sync_file) wait | The runtime's import of the dma-buf blocks until the producer's write is done (about 100 ms with the test's long writes), so a skipped acquire wait read no unfinished frame |

FFmpeg:

- The Vulkan decoder outputs NV12 as one image with two planes, tiled
  OPTIMAL, on all three GPUs. On RADV and ANV its memory is not exportable;
  on the NVIDIA driver it is. A frames context with
  `AV_VK_FRAME_FLAG_DISABLE_MULTIPLANE` and the export flags gives one
  exportable image per plane; a device copy (`vkCmdCopyImage()` of each
  plane, on the producer's queue) moves a decoded frame into it.
  `scale_vulkan` with nearest scaling as the copy is not bit-exact.
- Ubuntu's FFmpeg 8.0 frames pool did not export its memory; FFmpeg n9.0.2's
  did (the tests use n9.0.2).
- `AVVkFrame.sem[i]` / `sem_value[i]` are the images' timelines: the producer
  waits at `sem_value` and signals `sem_value + 1`, so an import waits at the
  current value and the release signals one more.

GStreamer 1.28: `gst_vulkan_memory_alloc()` fills `VkMemoryAllocateInfo` with
`pNext = NULL` (`gstvkmemory.c`), so memory of its image allocator cannot be
exported. A GStreamer element that hands frames to VMAFx must allocate
exportable images itself and wrap them
(`gst_vulkan_image_memory_wrapped()`); `GstVulkanImageMemory.barrier.parent`
carries the timeline (`semaphore`, `semaphore_value`).

Two platform behaviours shaped the tests:

- A producer that submits its next write waiting on a release value the
  library has not signalled yet stalls its queue: later submissions on it
  wait too (observed with the NVIDIA driver). The release canary therefore
  signals its own hold from the host, and the documentation tells producers
  to submit a reuse only after the release.
- The gfx1036 drops a dispatch now and then (the HIP lane's known defect,
  ADR-2092); the bit-exactness and fence tests repeat a failed cell up to
  three times on HIP and print every repeat.

## Measured results of the import

Recorded in the PR of ADR-2152 (cells, values and imports per backend, the
fence arms with and without the planted defect, and the vendor profiler
traces). Summary of the traces, each of an import-only run of
`test_vmafx_import_vulkan_<lane>_bitexact` (`VMAFX_TEST_IMPORT_ONLY=1`):

- CUDA, Nsight Systems 2026.3.2 `--trace=cuda`, 472 cells, 12056 imports:
  the library stream ran 15070 array-to-device and 4680 device-to-device
  copies and no host-to-device or device-to-host copy; elsewhere 120
  host-to-device copies of at most 64 KiB (the twins' tables) and the twins'
  result and term readbacks on their own streams.
- HIP, `rocprofv3 --memory-copy-trace --kernel-trace`, 472 cells: no memory
  copy on the library stream (its kernels: `copyBufferRectAligned`, the
  de-interleave and shift kernels, `ms_ssim_picture_to_float`); elsewhere 40
  host-to-device copies on `float_adm`'s stream and the twins' readbacks.
- SYCL, `sycl-trace --ur.call`, the 4K half (96 cells, 1152 imports): the
  imported memory is never a copy's source or destination and is read by 852
  kernel pointer arguments; the host sent 28 copies of 1648784 bytes in all
  (the largest 262148, tables), and the device's other copies are the twins'
  readbacks into host memory and device-to-device copies of their own buffers.

## Open

- Level Zero imports Vulkan timeline semaphores; a device-side wait and
  signal on the SYCL queue (`zeCommandListAppendWaitExternalSemaphoreExt()`)
  would drop the host wait of the SYCL lane.
- OPTIMAL images on HIP: measure every size and format (4K, P016) and
  whether RADV compresses the images the decoder copy writes before
  accepting them.
- Windows handles (`OPAQUE_WIN32`) are declared and refused until a Windows
  device runs the tests.

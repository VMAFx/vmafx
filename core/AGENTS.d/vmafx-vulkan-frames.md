---
paths:
  - core/src/vmafx/frame_import_vulkan.c
  - core/src/cuda/import_vulkan.c
  - core/test/vmafx_vulkan_producer.c
  - core/test/vmafx_vulkan_test_util.h
  - core/test/test_vmafx_import_vulkan*
invariant: Library makes no Vulkan call; CUDA imports opaque memory + timelines, SYCL/HIP take the dma-buf route; refusals named.
---
<!-- markdownlint-disable MD013 MD060 -->
# VMAFx Vulkan frame import (ADR-2152)

## Rebase-sensitive invariants

- Import only (Q-011): no Vulkan loader linked into the library, no Vulkan call in `core/src/`. `VULKAN_SEMAPHORE` fences: `vmafx_fence_wait()` / `vmafx_fence_destroy()` refuse by name (`fence.c`). Only tests (`vmafx_vulkan_producer.c`) link `dependency('vulkan')`.
- `frame_import_vulkan.c` = device-independent checks (handle type, tiling, flags, fd / size per plane, `plane_index` 0, LINEAR without modifier, rows fit `size`), PCI match (`desc.vulkan_pci`; backend without PCI -> `device`), `acquire_more` (only CUDA takes non-NONE), `vmafx_import_vulkan_as_dmabuf()` (SYCL / HIP: LINEAR or DRM tiling, memory must be a dma-buf per `vmafx_fd_is_dmabuf()`, else refused). Change a refusal -> change `test_vmafx_import_vulkan_api.c` + `test_refusals_named` + docs table (`docs/api/vmafx/index.md#vulkan-frames`) together.
- CUDA (`cuda/import_vulkan.c`): OPAQUE_FD only (driver imports dma-bufs on Tegra only, `cuda.h` 13.4). OPTIMAL -> mipmapped array at plane offset (U8 / U16, 2 channels for interleaved chroma); LINEAR -> mapped buffer -> DEVICE_POINTER path. Acquires waited on library stream (`cuWaitExternalSemaphoresAsync`, planted skip `VMAFX_TEST_SKIP_ACQUIRE_WAIT`). Release: `vmafx_frame_signal_on_release()` fences signalled on library stream behind last reader, then `done` event, retired list drained (bounded 4096) before the next map and at device close. Early-release canary signals on a fresh non-blocking stream (`vmafx_cuda_vulkan_release_early`).
- PCI location per backend: `VmafxDeviceInfo.pci` from `cuDeviceGetPCIBusId` (CUDA), `ext_intel_pci_address` (SYCL, `vmafx_sycl_rt.cpp` hands the string to C), `hipDeviceGetPCIBusId` (HIP); one parser for all three, `vmafx_parse_pci_bus_id()` (no `sscanf`, cert-err34). Cross-GPU import never read; refused.
- Tests per lane (`VMAFX_VK_LANE` 1 / 2 / 4, `core/test/meson.build` end block, suite `vulkan`): `_vulkan` (functional, two contexts, refusals), `_bitexact` (every exact cell == host upload; `VMAFX_TEST_ONLY`, `VMAFX_TEST_IMPORT_ONLY` for profiler traces), `_fence` (acquire + release arms with planted defects; release arm scores each frame in two contexts, planted release must be bad on 8/8 on SYCL / HIP, >= 1 on CUDA (one import copy); HIP import waits for the writer, T-HIP-DMABUF-IMPORT-WAITS-FOR-WRITER-2026-10-06), `_ffmpeg` (FFmpeg Vulkan decode + device copy into per-plane images; needs `libavutil/hwcontext_vulkan.h`). Device runs under the lane's `flock` with `timeout`; HIP repeats a differing cell (gfx1036 drops dispatches).
- Producer queues stall on a wait-before-signal: canaries signal their own holds from the host; never submit a reuse write waiting on a release value the library has not signalled.

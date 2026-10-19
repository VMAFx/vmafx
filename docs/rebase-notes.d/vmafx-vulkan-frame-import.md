## VMAFx Vulkan frame import (RC4 WP3 Vulkan lane, 2026-10-07)

`rc4/api-wp3-vulkan-import`, [ADR-2152](adr/2152-vmafx-vulkan-frame-import.md),
[ADR-2091](adr/2091-vmafx-sycl-device-frames.md),
[ADR-2092](adr/2092-vmafx-hip-device-frames.md). Lands after the SYCL and
HIP lanes; their single copies of the shared helpers
(`core/src/vmafx/sync_object.{c,h}`, `core/src/vmafx/release_events.c`, the
`vmafx_device_cells.h` / `vmafx_sycl_cells.h` tables on
`vmafx_exact_cells.h`) are the ones this lane builds on.

- A `SYNC_FILE` fence is destroyed by closing its descriptor (ADR-2091 item
  6, landed with the SYCL lane #2342); this lane's `fence.c` adds the
  `VULKAN_SEMAPHORE` refusals next to it, and a `GL_SYNC` fence is still
  refused (the producer's, `glDeleteSync()`).
- API (`core/api/vmafx.toml`, ABI 0.1.10): `VMAFX_MEMORY_VULKAN`,
  `VMAFX_FENCE_VULKAN_SEMAPHORE`, the enums `VmafxVulkanHandleType` and
  `VmafxVulkanTiling`, the flags `VmafxVulkanFlags`, `VmafxDeviceInfo.pci`,
  the `VmafxFrameImport` fields `acquire_more`, `vulkan_handle_type`,
  `vulkan_tiling`, `vulkan_flags`, `vulkan_pci`, and
  `vmafx_frame_signal_on_release()`. On a conflict in a generated file take
  either side and run `python3 scripts/codegen/vmafx-api.py --write`.
- New files: `core/src/vmafx/frame_import_vulkan.c` (the device-independent
  checks and the dma-buf translation), `core/src/cuda/import_vulkan.c` (the
  CUDA interop; `VmafxCudaVulkan` in `vmafx_cuda_internal.h`), the producer
  and tests `core/test/vmafx_vulkan_producer.{c,h}`,
  `vmafx_vulkan_test_util.h`, `test_vmafx_import_vulkan*.c`; the Vulkan tests
  are built once per device lane when `dependency('vulkan')` is found.
- Each lane reports its device's PCI location (`cuda/import_device.c`,
  `sycl/vmafx_sycl_rt.cpp`, `hip/import_device.c`), parsed by the one
  `vmafx_parse_pci_bus_id()` of `vmafx/frame_import_vulkan.c`, and
  translates a VULKAN descriptor before its own checks
  (`cuda/import_frame.c`, `sycl/import_frame.c`, `hip/import_frame.c`).
- `vmafx_exact_cells.h` holds `vc_import_only()` (`VMAFX_TEST_IMPORT_ONLY`),
  formerly a copy in each of the CUDA and HIP bit-exactness tests.
- No `libvmaf.h`, golden-data or FFmpeg patch impact.

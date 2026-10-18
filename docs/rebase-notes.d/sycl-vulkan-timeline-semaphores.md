## SYCL lane waits on and signals Vulkan timelines on its queue (2026-10-18)

- `core/src/sycl/import_frame.c`, `import_fence.c`, `vmafx_sycl_rt.{h,cpp}`:
  Vulkan timeline semaphores are imported per frame with DPC++ external
  semaphores, waited on the library queue before the first read and signalled
  behind the release join ([ADR-3352](../adr/3352-sycl-vulkan-timeline-on-queue.md)).
  Fork-only code, no upstream counterpart. **On sync**: keep the `dup()` of
  the descriptor before the import (the runtime closes the one it imports)
  and the `VULKAN_SEMAPHORE` bit of the SYCL fence-kind mask in
  `import_device.c`; `test_vmafx_import_vulkan_sycl_fence` guards both on an
  Arc device.

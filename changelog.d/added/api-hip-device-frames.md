- **VMAFx device frames on HIP (RC4, ADR-2092).** In a build with the HIP
  backend the VMAFx API creates HIP devices by index or from your stream
  (`vmafx_device_create` with `VMAFX_BACKEND_HIP`), scores a context on one
  (`vmafx_context_use_device`; features registered afterwards run on their
  HIP twins) and imports frames without a copy through the host
  (`vmafx_frame_import`): HIP device pointers at any offset and pitch,
  dma-bufs (`VMAFX_MEMORY_DMABUF`, imported as external memory), HIP arrays
  and OpenGL textures from a GLX context on the device's GPU; NV12, P010 and
  P016 are planarised on the device. Acquire fences of kind
  `VMAFX_FENCE_HIP_EVENT` are waited on by the device's stream;
  `VMAFX_FENCE_SYNC_FILE` (for example a dma-buf's exported sync_file) and
  `VMAFX_FENCE_GL_SYNC` are checked on the host and waited for by
  `vmafx_context_import_frame`. Release fences (`VMAFX_FENCE_HOST`,
  `VMAFX_FENCE_HIP_EVENT`) are signalled after the last reader in every
  context, and the release callback runs after them. `vmafx_fence_wait`
  waits on `VMAFX_FENCE_SYNC_FILE` and `VMAFX_FENCE_GL_SYNC` fences in every
  build. Imported frames score bit for bit as the same frames uploaded from
  the host. A HIP device has no frame pools, and a `VMAFX_FENCE_SYNC_FILE`
  release fence is refused (the ROCm runtime cannot signal one). See
  [HIP devices](docs/api/vmafx/index.md#hip-devices).

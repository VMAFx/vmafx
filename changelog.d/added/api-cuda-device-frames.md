- **VMAFx device frames on CUDA (RC4, ADR-1929, ADR-2023).** In a build with
  the CUDA backend the VMAFx API creates CUDA devices by index or from your
  context and stream (`vmafx_device_create` with `VMAFX_BACKEND_CUDA`,
  `vmafx_device_count`, `vmafx_device_info`), scores a context on one
  (`vmafx_context_use_device`; features registered afterwards run on their
  CUDA twins) and imports frames without a copy through the host
  (`vmafx_frame_import`): CUDA device pointers are read where they are, NV12,
  P010 and P016 are planarised on the device, CUDA arrays and OpenGL textures
  (the new `VMAFX_MEMORY_GL_TEXTURE`) are read out on the device. Acquire
  fences of kind `VMAFX_FENCE_CUDA_EVENT` are waited on by the device's
  stream, and the new `VMAFX_FENCE_GL_SYNC` orders a GL producer's rendering;
  release fences (`VMAFX_FENCE_HOST`, `VMAFX_FENCE_CUDA_EVENT`) are signalled
  after the last reader in every context, and the new release callback of
  `VmafxFrameImport` (`release`, `user`) lets a producer make its stream wait
  on the release event before it reuses its memory. CUDA frame pools
  (`vmafx_frame_pool_create` with a CUDA device) hand out device frames.
  Imported frames score bit for bit as the same frames uploaded from the
  host. ABI 0.1.4. See
  [CUDA devices](docs/api/vmafx/index.md#cuda-devices).

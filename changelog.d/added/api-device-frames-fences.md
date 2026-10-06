- **VMAFx device frames, fences and frame pools (RC4, ADR-1852, ADR-1929).**
  The VMAFx API gains the shared contract of zero-copy frame import:
  device enumeration and information (`vmafx_device_count`,
  `vmafx_device_info`, `vmafx_device_describe`, `vmafx_device_profile`; the
  size-prefixed `VmafxDeviceInfo` grows by the format envelope later),
  devices from external handles and profiling flags (`VmafxDeviceDesc`),
  attaching a device to a context (`vmafx_context_use_device`), fences of
  every kind (`VmafxFence`; `vmafx_fence_create`, `vmafx_fence_signal`,
  `vmafx_fence_wait`, `vmafx_fence_destroy`; the new status
  `VMAFX_E_TIMEOUT`), frame import with an acquire fence
  (`vmafx_frame_import`, `VmafxFrameImport`; NV12, P010 and P016 converted
  to planar, never through a host copy), release fences signalled after the
  last reader of a frame (`vmafx_frame_release_fence`), frame pools
  (`vmafx_frame_pool_create`, `vmafx_frame_pool_acquire`,
  `vmafx_frame_pool_destroy`), admission that names every refusing extractor
  (`vmafx_context_admit`) and the import rule of decision D8
  (`vmafx_context_import_frame`: one retry after a host wait, then a failure
  that names the import). This build implements host memory and host fences
  on the CPU device; the CUDA, SYCL, HIP and Metal imports follow behind the
  same functions. An imported frame scores bit for bit as the same frame
  created on the host. ABI 0.1.2. See
  [device frames and fences](docs/api/vmafx/index.md#device-frames-and-fences).

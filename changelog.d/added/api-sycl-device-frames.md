- **VMAFx device frames on SYCL (RC4, ADR-1929, ADR-2091).** In a build with
  the SYCL backend the VMAFx API creates SYCL devices on the Level Zero GPUs,
  by index or in the context of your `sycl::queue` (`vmafx_device_create` with
  `VMAFX_BACKEND_SYCL`), scores a context on one (`vmafx_context_use_device`;
  features registered afterwards run on their SYCL twins) and imports frames
  without a copy through the host (`vmafx_frame_import`): USM planes of any
  pitch, and on Linux dma-bufs (linear, Intel Y-tiled and Tile4, de-tiled on
  the device) and OpenGL textures (exported through EGL as dma-bufs). NV12,
  P010 and P016 are planarised on the device, and every SYCL twin, the chroma
  twins included, reads imported frames. Acquire fences of kind
  `VMAFX_FENCE_SYCL_EVENT` are waited on by the device, `VMAFX_FENCE_SYNC_FILE`
  and `VMAFX_FENCE_GL_SYNC` and a dma-buf's own write fences are checked on
  the host; release fences (`VMAFX_FENCE_HOST`, `VMAFX_FENCE_SYCL_EVENT`) and
  the release callback signal after the last reader in every context. SYCL
  frame pools hand out device frames. Imported frames score bit for bit as
  the same frames uploaded from the host. Windows shared textures and
  `sync_file` release fences are refused for now. `vmafx_fence_destroy`
  closes a `VMAFX_FENCE_SYNC_FILE` descriptor, on every device. See
  [SYCL devices](docs/api/vmafx/index.md#sycl-devices).

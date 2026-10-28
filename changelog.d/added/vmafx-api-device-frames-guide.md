- **Device-frames guide for the VMAFx C API (issue 2436).** A new page
  explains how GPU decoder frames reach the library without a host copy:
  `vmafx_frame_import()`, the memory kinds and fence kinds each backend
  accepts (CUDA, HIP, SYCL, Vulkan import), what Metal supports today, the
  release fence and the failures a refusal reports
  ([Device frames](docs/api/vmafx/device-frames.md)).

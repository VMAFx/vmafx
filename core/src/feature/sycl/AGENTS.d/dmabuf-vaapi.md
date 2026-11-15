---
paths:
  - core/src/sycl/dmabuf_import.cpp
  - core/src/feature/sycl/integer_motion_sycl.cpp
invariant: VAAPI / dmabuf zero-copy import — FFmpeg libvmaf_sycl pipeline.
area: runtime-and-kernels
---
<!-- markdownlint-disable MD013 MD060 -->
# VAAPI and dmabuf zero-copy import

- **VAAPI / dmabuf zero-copy import** — FFmpeg `libvmaf_sycl`
  filter (`ffmpeg-patches/0005-*.patch`) consumes
  `vmaf_sycl_import_va_surface`. Public-surface change touches
  patch file too — see CLAUDE.md §12 r14 +
  [ADR-0183](../../../../../docs/adr/0183-ffmpeg-libvmaf-sycl-filter.md).

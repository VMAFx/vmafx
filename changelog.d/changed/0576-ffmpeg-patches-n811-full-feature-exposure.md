- **chore(ffmpeg-patches):** Comprehensive feature-exposure sync for the
  `ffmpeg-patches/` stack: verified all 13 existing patches apply cleanly
  to FFmpeg n8.1.1 (the already-declared target), confirmed no deleted
  libvmaf symbols appear in any patch hunk, and added new **patch 0014**
  that exposes `VmafConfiguration.cpumask` and `.gpumask` as AVOptions on
  all libvmaf filter variants (`libvmaf`, `libvmaf_sycl`, `libvmaf_vulkan`,
  `libvmaf_metal`). Users can now write `libvmaf=cpumask=8` to cap dispatch
  at AVX2, or `gpumask=1` to disable CUDA, without rebuilding libvmaf.
  (ADR-0576)

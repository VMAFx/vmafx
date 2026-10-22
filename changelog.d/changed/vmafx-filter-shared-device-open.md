- **FFmpeg `vf_vmafx` shares device open and import finishing helpers.**
  `ffmpeg-patches/src/vf_vmafx.c` extracts `device_create_from_option()` for DRM and
  software-GPU device setup and `import_finish()` for CUDA and DRM release fences
  (HISS-19 dedupe item `ffmpeg-patches-src-1`). Behavior and error logging are unchanged.

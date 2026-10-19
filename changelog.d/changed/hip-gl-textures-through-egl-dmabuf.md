- **HIP imports OpenGL textures through EGL dma-buf export**
  ([ADR-2132](docs/adr/2132-hip-gl-textures-through-egl-dmabuf.md)): GL
  texture imports on a HIP device now work on the project's ROCm 10.1 (the
  runtime's GL interop could not read a mapped texture) and need an EGL
  context, not GLX. A texture exported in the driver's own tiling is copied
  on the GPU into a linear dma-buf and needs `VMAFX_IMPORT_ALLOW_COPY`;
  `libgbm.so.1` is needed for that copy. See
  [HIP devices](docs/api/vmafx/index.md#hip-devices).

## HIP GL textures through EGL dma-bufs (RC4 WP3 HIP follow-up)

`rc4/api-wp3-hip-gl-dmabuf`, [ADR-2132](adr/2132-hip-gl-textures-through-egl-dmabuf.md).

- `core/src/hip/import_gl.c` no longer calls `hipGraphics*`;
  `core/src/vmafx/egl_export.c` (shared, `internal.h` declarations) exports
  the textures and `import_gl()` in `core/src/hip/import_frame.c` imports them
  as a DMABUF frame. `vmafx_hip_gl_map()` / `vmafx_hip_gl_release()`,
  `VmafxHipGl` and `fill_failed()`'s GL branch are gone; a rebase that
  brings them back from ADR-2092 takes this side.
  `test_vmafx_import_hip_contract.py` holds the order (GL sync, GPU check,
  export, copy only with `VMAFX_IMPORT_ALLOW_COPY`, writers waited).
- `core/test/test_vmafx_import_hip_gl.c` is EGL, not GLX, and uses the new
  `core/test/vmafx_egl_test_util.h`, which the SYCL GL test can adopt.
- The SYCL lane (`rc4/api-wp3-sycl`) has its own EGL export in
  `core/src/sycl/import_gl.c`: when the lanes merge, fold it onto
  `egl_export.c` (one implementation, HISS-19).
- No `libvmaf.h`, ABI, golden-data or FFmpeg patch impact.

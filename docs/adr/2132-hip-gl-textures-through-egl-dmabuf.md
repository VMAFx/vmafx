<!-- markdownlint-disable MD013 MD060 -->
# ADR-2132: HIP imports GL textures through EGL dma-buf export; the runtime's GL interop is not used

- **Status**: Accepted
- **Date**: 2026-10-06
- **Deciders**: maintainer (RC4 work package 3 follow-up G6); RC4 WP3 HIP lane
- **Tags**: api, rc4, gpu, hip, opengl, zero-copy

## Context

[ADR-2092](2092-vmafx-hip-device-frames.md) imported GL textures on HIP through the
runtime's GL interop (`hipGraphicsGLRegisterImage()`), and measured that the
pinned ROCm 10.1.0 maps such a texture but cannot read it
(`T-HIP-ROCM10-GL-TEXTURE-READ-2026-10-06`), so every GL import was refused
there. OBS on Linux holds its frames as GL textures (#2238), so on an AMD GPU
with the pinned toolchain the zero-copy path did not exist. The same ADR
measured that dma-buf import works on 10.1, and
ADR-2091 (the SYCL lane) already reads GL textures on SYCL
by exporting each texture as a dma-buf through EGL
(`EGL_MESA_image_dma_buf_export`).

Measured on the gfx1036 (Mesa 26.0.8 in the pinned image, 2026-10-06): the
export of an `R8` 640x360 texture and of an `RG8` 320x180 texture succeeds and
reports modifier `DRM_FORMAT_MOD_INVALID`; read through HIP as linear rows,
223464 of 230400 and 106868 of 115200 samples differ, and the dma-bufs are
larger than linear rows need (245760 bytes for 230400): radeonsi exports its
own tiled layout. A device that reads linear rows cannot use it, and the AMD
tiling is not de-tiled on the device here.

## Decision

1. **Every HIP GL import goes through dma-buf.** `core/src/hip/import_gl.c`
   no longer touches `hipGraphics*`: the textures of the EGL context current
   on the importing thread are exported as dma-bufs
   (`core/src/vmafx/egl_export.c`) and the frame is imported as a `DMABUF`
   frame (ADR-2092 item 5). ROCm 7.2's interop is not kept: it read only the
   current GLX context and crashed after a failed first call, which is a
   second reason to leave it, and one path is one behaviour. EGL, GL and GBM
   entry points are resolved at run time.
2. **A texture the driver exports tiled is copied on the GPU, with the
   caller's consent.** When the exported modifier is not linear, the texture
   is blitted (one `glBlitFramebuffer()` inside the producer's context) into
   a linear dma-buf allocated with GBM on the context's render node, bound as
   a renderbuffer through an EGL image. This is a GPU copy, never a host
   copy, and needs `VMAFX_IMPORT_ALLOW_COPY`; without it the import is
   `VMAFX_E_NOTSUP` naming the flag. A driver that exports linear is read in
   place with no copy. The texture's format must be the plane's (`R8`,
   `GR88`, `R16`, `GR1616`), checked from the export.
3. **The producer's GL state is restored.** The copy changes the framebuffer
   and renderbuffer bindings, the scissor test and the colour mask, and puts
   them back.
4. **The GPU is checked.** The render node of the context's EGL display is
   resolved to its PCI location and compared with the importing device's;
   another GPU's dma-bufs are refused naming the device. A display without a
   DRM device is refused (the check is not skipped).
5. **Fences.** The GL sync acquire is checked on the host first
   (`vmafx_gl_sync_acquire()`, unchanged: unsignalled is `VMAFX_E_BUSY` and the
   import rule waits). After the export each dma-buf's pending writes (the
   kernel's implicit fences of the GL driver) are waited for at most 1 s.
6. **`T-HIP-ROCM10-GL-TEXTURE-READ-2026-10-06` is closed for vmafx.** The
   runtime defect stays real but nothing reads through it.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| EGL dma-buf export, GPU copy to linear when tiled (chosen) | Works on the pinned ROCm 10.1; one path for every runtime; the SYCL route | A GPU copy per tiled texture; GBM and framebuffer blits needed at run time | Chosen |
| EGL export read as it is | No copy | Wrong values on radeonsi (measured, 97 % of samples) | Rejected after measurement |
| De-tile AMD layouts in a HIP kernel | No GL work in the producer's context | Layout is per ASIC generation and not in the export (modifier invalid) | Not chosen |
| Keep HIP-GL for 7.2 and dma-buf for 10.1 | Zero-copy-er where it reads | Two paths; 7.2's crashes; GLX only | Not chosen: one path |
| GL memory objects from a HIP-owned buffer | No GBM | Needs GL_EXT_memory_object_fd on the producer's context and an exportable HIP allocation; unmeasured | Not chosen |
| Refuse GL on HIP | No code | OBS on AMD Linux has no zero-copy path (#2238) | Not chosen |

## Consequences

- **Positive**: GL frames import on the pinned ROCm 10.1 and score bit for
  bit as host-uploaded frames (`test_vmafx_import_hip_gl`: NV12 and P010, 42
  values each, 0 differing); no host copy (counter 0); the runtime's crashing
  GL interop and the GLX requirement are gone, an EGL context (surfaceless,
  Wayland, X11) is what the producer needs.
- **Negative**: every GL import on a tiling driver is one GPU blit per plane
  and a bounded host wait for the blit's fence; `VMAFX_IMPORT_ALLOW_COPY` is
  required there; GBM (`libgbm.so.1`) is needed at run time.
- **Neutral / follow-ups**: the SYCL lane carries its own EGL export
  (`core/src/sycl/import_gl.c`); folding it onto `egl_export.c` is a
  dedupe row for the merge of the lanes. A producer that can render into a
  linear dma-buf itself avoids the copy.

## References

- [ADR-2092](2092-vmafx-hip-device-frames.md), ADR-2091 (the SYCL lane), [ADR-2023](2023-vmafx-cuda-device-frames.md), [ADR-1929](1929-vmafx-device-frames-fences.md), [ADR-1897](1897-vmafx-abi-0x-numbering.md), [Research-2161](../research/2161-hip-gl-egl-dmabuf-tiling.md).
- `req` (RC4 WP3 follow-up, 2026-10-06): "HIP GL import routed through EGL dma-buf export because ROCm 10.1 cannot read GL textures ... GL sync object as the acquire fence ... Evidence on ROCm 10.1 in `vmafx-hip-lane:rocm10.1.0`".
- Tests: `core/test/test_vmafx_import_hip_gl.c`, `test_vmafx_import_hip_contract.py`.

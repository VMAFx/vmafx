<!-- markdownlint-disable MD013 MD060 -->
# Research-2161: What an EGL dma-buf export of a GL texture is on a gfx1036, and how HIP can read it

- **Status**: Active
- **Workstream**: [ADR-2132](../adr/2132-hip-gl-textures-through-egl-dmabuf.md), [ADR-2092](../adr/2092-vmafx-hip-device-frames.md)
- **Last updated**: 2026-10-06

## Question

The pinned ROCm 10.1 cannot read a GL texture through its GL interop
([Research-2159](2159-vmafx-hip-device-frames.md)), but imports dma-bufs. Can a
GL texture reach HIP as a dma-buf, and in what layout?

## Sources

`vmafx-hip-lane:rocm10.1.0` (ROCm 10.1.0, HIP 7.16.26385, Mesa 26.0.8) with
`libegl1`, `libegl-mesa0` and the EGL headers added; the gfx1036 iGPU render
node (`/dev/dri/renderD130`), a surfaceless GL 3.3 context on the EGL device
of that node, 2026-10-06. Probe: a C program that uploads an `R8` 640x360 and
an `RG8` 320x180 texture, exports each with
`eglExportDMABUFImageMESA()` and imports the descriptor with
`hipImportExternalMemory()` (opaque fd, the dma-buf's own size).

## Findings

| What | Result |
| --- | --- |
| Export | Succeeds; fourcc `R8` (`0x20203852`) and `GR88` (`0x38385247`), one plane, modifier `0x00ffffffffffffff` (`DRM_FORMAT_MOD_INVALID`), pitch 640 for both, sizes 245760 and 122880 bytes |
| HIP import of the export | `hipImportExternalMemory()` and mapping succeed |
| Content read as linear rows (pitch from the export) | 223464 of 230400 (R8) and 106868 of 115200 (RG8) samples differ: the layout is tiled; the size (384 rows of 640) matches a height padded to the tile, not the linear 360 rows |
| Linear copy | A GBM buffer (`GBM_BO_USE_LINEAR \| GBM_BO_USE_RENDERING`) bound as a renderbuffer through an EGL image and filled by one `glBlitFramebuffer()` from the texture: read through HIP, 0 samples differ in 42 values per context of NV12 and P010 frames (`test_vmafx_import_hip_gl`) |
| Host traffic of a GL session (`rocprofv3 --memory-copy-trace`) | Only host-to-device copies of the test's and the twins' own tables (46); no device-to-host copy; the blit runs in the GL context's queue |

## Conclusions

The export is readable only by something that knows the tiling, so HIP reads a
linear copy made by the GPU in the producer's own GL context. Which Mesa
versions or drivers export linear is not measured here; the code reads a
linear export in place when the modifier says so.

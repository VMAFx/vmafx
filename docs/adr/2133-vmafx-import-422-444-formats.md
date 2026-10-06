<!-- markdownlint-disable MD013 MD060 -->
# ADR-2133: Device import takes 4:2:2 and 4:4:4: semi-planar NV16 / NV24 family and packed Y210 / Y410, one CPU reference, converted on the device

- **Status**: Accepted
- **Date**: 2026-10-06
- **Deciders**: maintainer (RC4 work package 3 follow-up G7); RC4 WP3
- **Tags**: api, rc4, gpu, zero-copy, abi

## Context

[ADR-1929](1929-vmafx-device-frames-fences.md) fixed the import formats as
planar layouts plus NV12 / P010 / P016. Hardware decoders and encoders also
produce 4:2:2 and 4:4:4 (work package index G7). Those frames were refused
with `VMAFX_E_NOTSUP`, so a pipeline that decodes them had to download them.
What the decoders of the development host emit was measured with FFmpeg 9
(`AVHWFramesContext.sw_format` of the first frame, 640x360 H.264 and HEVC
streams, `docs/api/vmafx/index.md`): NVDEC (RTX 4090) decodes HEVC 4:4:4 only,
as `yuv444p` at 8 bits and as `yuv444p10msble` / `yuv444p12msble` at 10 and
12 bits (planar 16-bit words with the sample in the top bits); the Arc A380's
VAAPI decodes HEVC 4:2:2 and 4:4:4 as `yuyv422`, `y210le`, `y212le`, `vuyx`,
`xv30le`, `xv36le` (packed) and none of it from H.264; the gfx1036's VAAPI
decodes 4:2:0 only. None emits NV16 / P210 / NV24 / P410. The conversions
are the same kind as the existing ones: every output sample is one input
sample, shifted and masked.

## Decision

1. **Additive formats (ADR-1897, ABI 0.1.5).** `VmafxPixelFormat` gains the
   semi-planar `NV16`, `P210`, `P216`, `NV24`, `P410`, `P416`; the packed
   `Y210`, `Y212`, `YUYV422` (4:2:2), `Y410` (also Intel's XV30), `XV36`,
   `VUYX` (4:4:4); and `YUV444P_MSB` (planar 16-bit words with the `bpc` top
   bits, 9 to 16). The frames they make are YUV422P / YUV444P at the format's
   depth. Planar 4:2:2 / 4:4:4 device frames were already imported.
2. **One CPU reference (HISS-19).** `core/src/vmafx/import_convert.h` holds
   `vmafx_import_read_plane()` and its plan type `VmafxImportRead` (source
   plane, step, offset, element bytes, shift, mask); `frame_import.c` plans
   the read of each plane of each layout of the table
   (`vmafx_import_plane_read()`). The host import, the Metal IOSurface reader
   (`vmaf_metal_read_plane()` forwards) and any host input of these layouts
   use it.
3. **Device conversion.** The semi-planar layouts need no new kernel: the
   de-interleave and shift kernels take the chroma plane's own size. The
   packed layouts and `YUV444P_MSB` use one new kernel,
   `vmafx_import_gather` (`import_convert_kernels.h`, nvcc and hipcc), that
   performs the same plan as the reference once per output plane. A packed or
   MSB layout is linear words: in a device array or GL texture it is refused
   naming the memory kind. Every conversion is integer work only, so an
   imported frame scores bit for bit as the same frame created on the host.
4. **Per backend.** CUDA and HIP take the formats with this ADR. The SYCL
   kernels and the Metal formats follow in their lanes (the Metal request is
   `requests/WP3-formats-2.md` of the work package directory).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Semi-planar and packed, one reference, kernels per backend (chosen) | Covers what decoders emit; bit-exact by construction | Two kernels per backend for the packed layouts | Chosen |
| Semi-planar only | No new kernels | The frames the Arc and NVDEC emit (packed Intel layouts, MSB planar words) still refused | Not chosen: measured to be what decoders emit |
| Convert on the host | One implementation | A host copy: the rule of ADR-1929 forbids it | Rejected |
| A second reference next to Metal's | No Metal change | Two behaviours (HISS-19) | Not chosen: Metal's reader forwards |

## Consequences

- **Positive**: NVDEC 4:4:4 (`yuv444p`, MSB words) and the Arc's VAAPI
  4:2:2 / 4:4:4 frames (YUY2, Y210 / Y212, VUYX, XV30, XV36) import without a
  host copy and score as host frames (CUDA, HIP; `test_vmafx_import_cuda_formats`,
  `test_vmafx_import_hip_formats`).
- **Negative**: ABI 0.1.5 (additive); a new feature that refuses a depth or
  chroma layout (`psnr_hvs` at 16 bits) refuses the imported frame exactly as
  the host frame.
- **Neutral / follow-ups**: SYCL and Metal kernels; HOST-memory inputs of the
  same layouts through `vmafx_import_read_plane()` (work package 13).

## References

- [ADR-1929](1929-vmafx-device-frames-fences.md), [ADR-1897](1897-vmafx-abi-0x-numbering.md), [ADR-1679](1679-metal-iosurface-biplanar-import.md), [ADR-2023](2023-vmafx-cuda-device-frames.md), ADR-2092.
- `req` (RC4 WP3 follow-up, 2026-10-06): "Device import accepts NV12 / P010 / P016 only ... Add, additively (ADR-1897): semi-planar NV16 / P210 / P216 and NV24 / P410 / P416, planar device 4:4:4 where a decoder emits it, and packed Y210 / Y410 if a backend can de-interleave them on the device. Same rule as NV12 / P010: de-interleave and the 10-bit shift only, bit-exact with host upload."
- Tests: `core/test/test_vmafx_import_convert.c`, `test_vmafx_import_bitexact.c`, `test_vmafx_import_cuda_formats.c`, `test_vmafx_import_hip_formats.c`.

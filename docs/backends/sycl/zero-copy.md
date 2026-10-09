# SYCL zero-copy imports and picture pre-allocation

This page is for callers that hand decoded frames to the SYCL backend: the
FFmpeg `libvmaf_sycl` filter, or a program of your own that owns a decoder. It
says which ingestion path to use, the two rules that keep 10-bit QSV input
correct, and how the pre-allocated picture pool works. Back to the
[SYCL overview](overview.md).

## Ingestion paths

| Path | Entry point | Platform | Copies |
| --- | --- | --- | --- |
| Host pictures | `vmaf_read_pictures()` | all | One upload of both luma planes per frame into the state's shared frame |
| Pre-allocated pool | `vmaf_sycl_preallocate_pictures()`, `vmaf_sycl_picture_fetch()` | all | None when the decoder writes device USM |
| VA-API dmabuf | `vmaf_sycl_dmabuf_import()`, `vmaf_sycl_import_va_surface()` | Linux | None (zero-copy) |
| D3D11 staging | `vmaf_sycl_import_d3d11_surface()` | Windows | Two PCIe copies; not zero-copy |
| Plane upload | `vmaf_sycl_upload_plane()` | all | One host-to-device copy; returns when it has completed |
| VMAFx device frames | `vmafx_frame_import()` / `vmafx_context_import_frame()` on a SYCL device | all (USM); Linux (dma-buf, GL texture) | None; every SYCL twin reads the frame, chroma included |

The signatures are in
[`libvmaf_sycl.h`](../../../core/include/libvmaf/libvmaf_sycl.h) and the
programmatic surface in [api/gpu.md](../../api/gpu.md#sycl).

## VA-API dmabuf import

When the input is a VA-API surface, for example from a QSV-decoded FFmpeg
frame, the backend imports the dmabuf directly through
`ext::oneapi::experimental::external_memory`, with no CPU upload. See
[`dmabuf_import.cpp`](../../../core/src/sycl/dmabuf_import.cpp).

The fast path is Linux-only. It is gated on `#ifndef _WIN32`, and on Windows
`vmaf_sycl_dmabuf_import` and `vmaf_sycl_import_va_surface` return `-ENOSYS`
so the caller falls back to the D3D11 staging path. DMA-BUF is a Linux kernel
interface (`ZE_EXTERNAL_MEMORY_TYPE_FLAG_DMA_BUF`); Level Zero on Windows uses
NT handles instead.

### Zero-copy import scores luma-only features

The zero-copy import delivers luma only and hands the extractors no host
pictures. `vmaf_read_pictures_sycl()` therefore checks every registered
feature extractor before it counts a frame
([ADR-1688](../../adr/1688-sycl-zero-copy-luma-only-admission.md)):

| Runs on the zero-copy path | Refused with `-ENOTSUP` and named |
| --- | --- |
| `adm_sycl`, `vif_sycl`, `motion_sycl`, `motion_v2_sycl`, `cambi_sycl`, `float_moment_sycl` | `speed_chroma_sycl` (the default model's `speed_chroma_uv`), `speed_temporal_sycl`, `ciede_sycl`, `ssimulacra2_sycl`, `float_ms_ssim_sycl`, `float_ssim_sycl`, `integer_ssim_sycl`, `float_psnr_sycl`, `float_motion_sycl`, `float_vif_sycl`, `float_adm_sycl` |
| `psnr_sycl`, `psnr_hvs_sycl` with `enable_chroma=false` | `psnr_sycl`, `psnr_hvs_sycl` with `enable_chroma` (the default), `motion_sycl` with `motion_add_uv=true` |
| | any CPU extractor (no SYCL twin, or a twin that cannot honour the model's options) |

So `vmaf_v0.6.1` runs zero-copy and gives the CPU's per-frame scores. The
default model `vmaf_v1.0.16_3d0h` does not run zero-copy: the first frame
fails with a libvmaf error that names `speed_chroma_sycl`. With FFmpeg, score it
through the bridge below (`hwdownload,format=nv12` and the `libvmaf` filter's
`sycl_device` option).

Measured on an Arc A380 (xe driver, FFmpeg n9.0.2 with the series, QSV decode
of the Netflix 576x324 pair as H.264) before the check existed:

- `vmaf_v1.0.16_3d0h` failed with `vmaf_read_pictures_sycl failed: -22` and no
  word about chroma;
- `feature=name=float_psnr_sycl` ended FFmpeg with a segmentation fault (the
  twin dereferenced the missing picture);
- `motion_sycl` with `motion_add_uv=true` reported `integer_motion2_mau`
  equal to `integer_motion2` (4.257894 on frame 1, where the host path gives
  5.536504), because the U and V it adds were never imported;
- `feature=name=float_psnr` (the CPU extractor) was skipped on every frame,
  and the result had no `float_psnr` and no error.

This path stays luma-only. Frames imported through the VMAFx API carry the
chroma as well and every SYCL twin reads them; see
[VMAFx device frames](#vmafx-device-frames) below.

## VMAFx device frames

The VMAFx API imports a whole frame, luma and chroma, on a SYCL device
([ADR-2091](../../adr/2091-vmafx-sycl-device-frames.md)): USM of the device's
SYCL context, and on Linux dma-bufs (linear, Intel Y-tiled or Tile4) and
OpenGL textures (through an EGL dma-buf export). NV12 / P010 / P016 are
planarised and tiled planes de-tiled on the device with the same address
math as `vmaf_sycl_import_va_surface()` (`core/src/sycl/detile.h`); every
SYCL twin, the chroma twins included, copies the planes it reads on the
device. Imported frames score bit for bit as the same frames uploaded from
the host, for every SYCL twin declared exact. SYCL events, `sync_file`
descriptors and GL syncs order the producer's writes; a release fence (host or
SYCL event) and a release callback tell the producer when it may write into
the memory again. The calls, the descriptors and the fences are in
[SYCL devices](../../api/vmafx/index.md#sycl-devices).

What is not imported yet: Windows shared textures (`VMAFX_MEMORY_WIN32_SHARED`
is refused, the D3D11 staging path above remains), and a `sync_file` release
fence (a SYCL event or host fence is returned instead).

## D3D11 staging-texture import (Windows)

`vmaf_sycl_import_d3d11_surface` accepts an `ID3D11Texture2D*` from a Windows
decoder (MediaFoundation, DXVA2, the Direct3D11 VideoProcessor). The
implementation:

1. creates a staging texture with `D3D11_USAGE_STAGING` and
   `D3D11_CPU_ACCESS_READ`,
2. calls `CopyResource` to pull the GPU surface into the staging texture,
3. maps the staging texture for CPU read, and
4. forwards the mapped pointer and row pitch to `vmaf_sycl_upload_plane`.

This is **not zero-copy**: throughput is bounded by PCIe upstream (the staging
map) and PCIe downstream (the SYCL host-to-device copy). A zero-copy
equivalent would need DXGI NT-handle sharing and DPC++ D3D11 interop, which
oneAPI does not document as of 2025.1. See
[`d3d11_import.cpp`](../../../core/src/sycl/d3d11_import.cpp) and ADR-0103.

## QSV / VA-API zero-copy: 10-bit correctness (ADR-1121)

The `libvmaf_sycl` FFmpeg filter runs VMAF directly on QSV-decoded VA-API
surfaces with no host round-trip. Two requirements must hold for it to produce
correct scores on a 10 or 12-bit pair. Get either wrong and you get
`VMAF score: nan` with a wildly inflated `integer_motion`.

### Give each decoder its own QSV session (required)

If both decoders are created against the **same** `-hwaccel_device`, FFmpeg
shares one `AVHWFramesContext` → one `mfxSession` → one VA surface pool between
them. The two decoders then write into the **same physical `VASurfaceID`s**, so
the reference decoder overwrites the surface the distorted decoder just produced
(decode-order look-ahead). The filter reads reference content where it expects
distorted content. Symptom: a checksum/score pattern where
`sycl_dis[N] == sycl_ref[N±1]`.

The fix is a usage contract — libvmaf cannot see FFmpeg's session topology from
inside the filter, so it is **not** auto-detected. Give **each** input its own
QSV device:

```bash
ffmpeg \
  -init_hw_device drm=drm0:/dev/dri/renderD128 \
  -init_hw_device vaapi=va0@drm0 \
  -init_hw_device qsv=qsv_ref@va0 \
  -init_hw_device qsv=qsv_dis@va0 \
  -hwaccel qsv -hwaccel_output_format qsv -hwaccel_device qsv_dis -c:v av1_qsv -i dis.mkv \
  -hwaccel qsv -hwaccel_output_format qsv -hwaccel_device qsv_ref -c:v hevc_qsv -i ref.mkv \
  -lavfi '[0:v][1:v]libvmaf_sycl=log_fmt=csv:log_path=out.csv' \
  -frames:v 500 -f null -
```

A single shared `qsv` device silently reintroduces the contamination.

### Each input is imported with its own VA display; a failed import stops the filter

`libvmaf_sycl` reads the VA display of each input's QSV session and imports
that input's surfaces with it
([ADR-1761](../../adr/1761-sycl-filter-import-retry-then-fail.md)). Before
this, the reference surfaces were imported with the distorted input's display.
On an Arc A380 with the two decoders on two VA devices
(`-init_hw_device vaapi=va0:... -init_hw_device vaapi=va1:...`), that gave 0 of
24 frames equal to the CPU and a plausible-looking pooled score (54.914218
against 53.419552), without an error. When a surface ID did not exist in the
other display, it gave the `invalid VASurfaceID` import failures that the
filter used to skip.

A failed import is tried again, three tries in all with 1 ms between them, and
then stops the filter with an error naming the frame. A filter that stopped
prints no pooled score. `ffmpeg-patches/test/check-sycl-import-retry.sh`
injects transient and persistent failures to check both outcomes.

### P010/P012 pixels are normalized in the import

VA-API stores 10-bit (P010) / 12-bit (P012) samples **MSB-aligned**
(`V_MSB = V_LSB << (16 − bpc)`), while the VMAF feature kernels expect
LSB-aligned `bpc`-bit integers. The CPU `libvmaf` path never sees this because
FFmpeg auto-converts `P010LE → YUV420P10LE` (a `>> 6` shift) to satisfy the
filter's pixel-format list.

The SYCL import does the equivalent luma-only `>> (16 − bpc)` shift — **fused
into the Tile4 / Y-tiled de-tile kernel** on the QSV hot path (no extra GPU
pass), and a standalone `launch_p010_normalize()` kernel on the rare LINEAR /
readback fallbacks. It is a no-op for 8-bit NV12.

This is internal — no user action required — but explains why a hand-rolled VA
import that skips it sees `integer_motion` inflated by exactly $2^{16 - \mathrm{bpc}}$
(64× at 10-bit).

## Picture pre-allocation

`vmaf_sycl_preallocate_pictures()` + `vmaf_sycl_picture_fetch()` back a
2-deep ring of USM-backed `VmafPicture` instances that callers hand to
`vmaf_read_pictures()`. Three modes:

| `pic_prealloc_method` | Backing | Use case |
| --- | --- | --- |
| `VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_NONE` | No pool; `vmaf_sycl_picture_fetch` falls back to host `vmaf_picture_alloc` | CPU-fed pipelines, test harnesses |
| `VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_DEVICE` | `sycl::malloc_device` (GPU-resident) | Zero-copy decoder interop (decoder writes directly into device USM) |
| `VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_HOST` | `sycl::malloc_host` (coherent, CPU-visible) | Decoders that must write from the CPU but want pool reuse |

The pool depth (2) matches the double-buffered shared-frame upload in
`VmafSyclState`, so frame N+1 can start filling slot 1 while frame N's
compute still consumes slot 0. The caller owns the ref returned by
`vmaf_sycl_picture_fetch` and must release it via `vmaf_picture_unref` when
done with it; the pool retains its own ref until `vmaf_close()` returns exactly
zero. A nonzero close status retains the pool with the teardown-only context.

Minimal example:

```c
VmafSyclPictureConfiguration cfg = {
    .pic_params = { .w = 1920, .h = 1080, .bpc = 8, .pix_fmt = VMAF_PIX_FMT_YUV420P },
    .pic_prealloc_method = VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_DEVICE,
};
vmaf_sycl_preallocate_pictures(vmaf, cfg);

for (unsigned i = 0; i < n_frames; i++) {
    VmafPicture ref, dis;
    vmaf_sycl_picture_fetch(vmaf, &ref);   /* device USM, caller writes */
    vmaf_sycl_picture_fetch(vmaf, &dis);
    /* ... fill ref.data[0] and dis.data[0] via decoder/upload ... */
    vmaf_read_pictures(vmaf, &ref, &dis, i);
}
vmaf_read_pictures(vmaf, NULL, NULL, 0);
```

See [ADR-0101](../../adr/0101-sycl-usm-picture-pool.md) for the design
rationale (Y-plane only, pool depth 2, refcount semantics).

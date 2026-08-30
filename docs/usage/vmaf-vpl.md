# `vmaf_vpl` — Intel VPL zero-copy VAAPI→SYCL pipeline (developer tool)

`vmaf_vpl` is a developer-only binary that drives the libvmaf SYCL backend
through Intel's Video Processing Library (VPL) for zero-copy
VAAPI→SYCL frame transfer. It exists to exercise the
`libvmaf_sycl_dmabuf_import` path end-to-end against a real Intel GPU
without going through FFmpeg.

The tool is **built but not installed**. It only compiles when SYCL +
Intel VPL + libva + libva-drm are all present at configure time. The
canonical invocation is from the build tree
(`./build/core/tools/vmaf_vpl`). For most users the
[`vmaf_libvmaf_sycl` FFmpeg filter](ffmpeg.md#dedicated-sycl-filter-libvmaf_sycl)
is the right entry point; `vmaf_vpl` is for libvmaf SYCL contributors
debugging the import path itself.

## Build prerequisites

1. SYCL toolchain (Intel oneAPI 2025.3+ with `icpx`).
2. Intel VPL runtime (`libvpl-dev` on Ubuntu 24.04/26.04, or oneAPI bundle).
3. `libva-dev` + `libva-drm-dev` for the VAAPI surface input path.

If any of the above is missing, meson silently skips the
`vmaf_vpl` target — `meson setup build` will succeed without it and
the binary will not appear under `build/core/tools/`.

## Input format

`vmaf_vpl` uses VPL (not FFmpeg) for decoding and only handles
**elementary bitstreams**: `.h264` / `.264` / `.avc` for H.264,
`.h265` / `.265` / `.hevc` for H.265 (the default), `.av1` / `.ivf` /
`.obu` for AV1, and `.vp9` for VP9. Container files (`.mp4`, `.mkv`,
`.webm`) are not demuxed — extract the elementary stream first:

```bash
# H.265 from MP4
ffmpeg -i input.mp4 -c:v copy -bsf:v hevc_mp4toannexb output.h265

# H.264 from MP4
ffmpeg -i input.mp4 -c:v copy -bsf:v h264_mp4toannexb output.h264
```

The codec is inferred from the file extension; unknown extensions default
to H.265.

## Flags

| Flag | Type | Default | Purpose |
| --- | --- | --- | --- |
| `--ref FILE` | string | — (required) | Reference elementary bitstream. |
| `--dis FILE` | string | — (required) | Distorted elementary bitstream. |
| `--model NAME` | string | `vmaf_v0.6.1` | VMAF model name (built-in lookup) or file path (fallback). |
| `--frames N` | uint | `0` (all frames) | Stop after N frames. |
| `--device N` | int | `0` | SYCL device index. |
| `--render-node PATH` | string | `/dev/dri/renderD128` | VA-API DRM render node. |
| `--fallback` | flag | off | Fall back to host upload when DMA-BUF zero-copy import fails. |
| `-h` / `--help` | — | — | Print usage and exit. |

## Smoke invocation

```bash
./build/core/tools/vmaf_vpl \
  --ref testdata/ref_576x324_48f.h265 \
  --dis testdata/dis_576x324_48f.h265 \
  --model vmaf_v0.6.1 \
  --frames 48 \
  --device 0
```

Successful runs print per-frame feature scores (first five frames) and
the mean pooled VMAF score on stdout, then exit 0. If DMA-BUF import
fails (older kernel without Level Zero VA import, or a DRM render node
mismatch), re-run with `--fallback` to confirm the SYCL backend itself
is healthy and isolate the issue to the import path.

## Status

The tool tracks ADR-0183 (FFmpeg `libvmaf_sycl` filter) — both share
the same SYCL dmabuf-import primitive. `vmaf_vpl` exists primarily as
a contributor regression-test entry point so the import path can be
debugged without an FFmpeg build round-trip. There is no plan to
install the binary; if you need a user-facing SYCL entry point use
the FFmpeg filter.

## Related

- [`ffmpeg.md`](ffmpeg.md) — FFmpeg `libvmaf_sycl` filter (the
  user-facing equivalent).
- [`docs/api/gpu.md`](../api/gpu.md) — `vmaf_sycl_dmabuf_import` C API.
- [ADR-0183](../adr/0183-ffmpeg-libvmaf-sycl-filter.md) — SYCL filter
  shipping policy.

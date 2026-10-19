# `vmaf_vpl` — Intel VPL zero-copy VAAPI→SYCL pipeline (developer tool)

`vmaf_vpl` is a developer-only binary that drives the libvmaf SYCL backend
through Intel's Video Processing Library (VPL) for zero-copy
VAAPI→SYCL frame transfer. It exists to exercise the
`libvmaf_sycl_dmabuf_import` path end-to-end against a real Intel GPU
without going through FFmpeg.

The tool is **built but not installed**. It only compiles when SYCL +
Intel VPL + libva + libva-drm are all present at configure time. The
canonical invocation is from the build tree
(`./build/tools/vmaf_vpl`). For most users the
[`libvmaf_sycl` FFmpeg filter](ffmpeg.md#libvmaf_sycl)
is the right entry point; `vmaf_vpl` is for libvmaf SYCL contributors
debugging the import path itself.

## Build prerequisites

1. SYCL toolchain (Intel oneAPI 2026.1 with `icpx`, the version pinned in
   `build-config.env`).
2. Intel VPL runtime (`libvpl-dev` on Ubuntu 24.04/26.04, or oneAPI bundle).
3. `libva-dev` + `libva-drm-dev` for the VAAPI surface input path.

If any of the above is missing, meson silently skips the
`vmaf_vpl` target — `meson setup build` will succeed without it and
the binary will not appear under `build/tools/`.

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
| `--model NAME` | string | `vmaf_v1.0.16_3d0h` | VMAF model name (built-in lookup) or file path (fallback). |
| `--frames N` | uint | `0` (all frames) | Stop after N frames. |
| `--device N` | int | `0` | SYCL device index. |
| `--render-node PATH` | string | `/dev/dri/renderD128` | VA-API DRM render node. |
| `--fallback` | flag | off | Fall back to host upload when DMA-BUF zero-copy import fails. |
| `-h` / `--help` | — | — | Print usage and exit. |

## Smoke invocation

No elementary-stream fixtures ship in the repository. Encode the tracked YUV
clips to H.265 first, then run the tool:

1. Create the two bitstreams:

    ```bash
    for n in ref dis; do
      ffmpeg -f rawvideo -pixel_format yuv420p -video_size 576x324 -framerate 24 \
        -i testdata/${n}_576x324_48f.yuv -c:v libx265 ${n}_576x324_48f.h265
    done
    ```

2. Run `vmaf_vpl` against them:

    ```bash
    ./build/tools/vmaf_vpl \
      --ref ref_576x324_48f.h265 \
      --dis dis_576x324_48f.h265 \
      --model vmaf_v0.6.1 \
      --frames 48 \
      --device 0
    ```

A successful run prints per-frame feature scores (the first five frames) and the
mean pooled VMAF score on stdout, then exits 0.

If DMA-BUF import fails (an older kernel without Level Zero VA import, or a DRM
render node mismatch), re-run with `--fallback`. That confirms the SYCL backend
itself is healthy and isolates the problem to the import path.

!!! note "`--help` shows an older model default"
    The tool's `--help` text and header comment still say
    `--model` defaults to `vmaf_v0.6.1`. The code default is
    `vmaf_v1.0.16_3d0h`, as in the flag table above.

## Limits

A single `vmaf_vpl` frame decode is capped at 60 000 `DecodeFrameAsync`
attempts:

- On a device that keeps reporting `MFX_WRN_DEVICE_BUSY`, the attempts are 1 ms
  apart, so the cap is the same 60 s ceiling the tool gives each sync
  operation.
- Attempts that only refill the bitstream do not sleep and are charged against
  the same budget, so the bound is on attempts rather than exactly on wall
  clock.
- On exhaustion the tool prints
  `DecodeFrameAsync yielded no frame after 60000 attempts`, reports
  `Decode error at frame N` and stops, instead of retrying a wedged device
  forever.

The ceiling contract, frame ordering, and status classification are formally
verified and protected by unit tests
(`core/tools/test/test_vmaf_vpl_decode_ceiling.c`), hardware smoke tests
(`core/tools/test/test_vmaf_vpl_hardware_smoke.sh`), and physical Intel Arc
hardware validation ([ADR-1900](../adr/1900-vpl-decode-ceiling-contract.md);
closing `T-VPL-DECODE-CEILING-UNVERIFIED-2026-09-21` in [state.md](../state.md)).
Decoded frames returned alongside warning statuses (such as
`MFX_WRN_VIDEO_PARAM_CHANGED`) are delivered cleanly, and transient
`MFX_WRN_ALLOC_TIMEOUT_EXPIRED` retries under the back-off schedule.

## Status

The tool tracks ADR-0183 (FFmpeg `libvmaf_sycl` filter); both share the same
SYCL dmabuf-import primitive. `vmaf_vpl` is a contributor regression-test entry
point, so the import path can be debugged without an FFmpeg build round-trip.
There is no plan to install the binary; for a user-facing SYCL entry point, use
the FFmpeg filter.

!!! note "Cleanup is fail-closed"
    `vmaf_vpl` retries `vmaf_close()` once. If the second attempt fails, it
    reports the negative error, exits non-zero and keeps the model and imported
    SYCL state alive until process exit.

## Related

- [`ffmpeg.md`](ffmpeg.md) — FFmpeg `libvmaf_sycl` filter (the
  user-facing equivalent).
- [`docs/api/gpu.md`](../api/gpu.md) — `vmaf_sycl_dmabuf_import` C API.
- [ADR-0183](../adr/0183-ffmpeg-libvmaf-sycl-filter.md) — SYCL filter
  shipping policy.

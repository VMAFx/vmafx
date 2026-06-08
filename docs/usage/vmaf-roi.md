<!-- markdownlint-disable MD013 MD060 -->
# vmaf-roi — saliency-driven ROI sidecars for x265 / SVT-AV1

`vmaf-roi` is a sidecar binary that consumes a per-frame saliency map and
emits an encoder-native per-CTU QP-offset file. It complements
[`mobilesal`](../ai/models/mobilesal.md) (the scoring-side saliency
extractor): same model, two surfaces — scoring the residual vs steering
the encoder.

> **Binary name note:** Built and installed as `vmaf_roi` (underscore,
> per `core/tools/meson.build`). Throughout this page the
> `vmaf-roi` (hyphen) form refers to the same binary; if you typed
> `vmaf-roi` and got "command not found", fall back to `vmaf_roi`.

This is **T6-2b** (sidecar). T6-2a shipped the in-libvmaf saliency
extractor.

## What it produces

For every CTU in a frame the tool emits a signed integer offset:

```text
qp_offset = clamp(-strength * (2 * saliency - 1), -12, +12)
```

- High saliency (eyes, faces, focal subject) → **negative** offset → encoder spends more bits there.
- Low saliency (background, periphery) → **positive** offset → encoder saves bits.
- Neutral saliency (≈ 0.5) → **zero** offset → no change.

## Build

`vmaf-roi` is built whenever `-Denable_tools=true` (the default):

```bash
meson setup build -Denable_cuda=false -Denable_sycl=false -Denable_tools=true
ninja -C build tools/vmaf_roi
```

The binary depends only on libvmaf's public DNN surface
([`libvmaf/dnn.h`](../../core/include/libvmaf/dnn.h)); when libvmaf is
built with `-Denable_dnn=disabled` the `--saliency-model` path returns
`-ENOSYS` and the tool falls back to a deterministic radial placeholder
useful only for smoke-testing the sidecar plumbing.

## Synopsis

```text
vmaf-roi --reference REF.yuv --width W --height H \
         --frame N --output qpfile.txt \
         [--pixel_format 420|422|444] [--bitdepth 8|10|12|16] \
         [--ctu-size 8..128] [--encoder x265|svt-av1] \
         [--strength FLOAT] [--saliency-model model.onnx]
```

Required flags:

| Flag             | Meaning                                              |
|------------------|------------------------------------------------------|
| `--reference`    | Raw planar YUV input. Read with no demuxer.          |
| `--width`        | Frame width in luma samples (≤ 16 384).              |
| `--height`       | Frame height in luma samples (≤ 16 384).             |
| `--frame`        | 0-based frame index inside the YUV file.             |
| `--output`       | Destination path; `-` writes to stdout.              |

Optional flags:

| Flag                | Default | Description                                                                 |
|---------------------|---------|-----------------------------------------------------------------------------|
| `--pixel_format`    | `420`   | One of `420` / `422` / `444`. Saliency reads luma only; chroma is skipped.  |
| `--bitdepth`        | `8`     | One of `8`, `10`, `12`, or `16`. High-bit-depth planar YUV uses little-endian 16-bit containers; luma is downscaled to the 8-bit DNN contract. |
| `--ctu-size`        | `64`    | Luma samples per CTU side. Range `8..128`. Use 64 for x265, 64 for SVT-AV1. |
| `--encoder`         | `x265`  | Selects sidecar format: `x265` (ASCII) or `svt-av1` (binary `int8_t`).      |
| `--strength`        | `6.0`   | QP-offset gain. Output is clamped to ±12 regardless of strength.            |
| `--saliency-model`  | *unset* | Path to a tiny ONNX `[1, 1, H, W]` luma → saliency model.                   |

## Sidecar formats

### x265 (`--encoder x265`)

ASCII grid, one row per CTU row, space-separated signed offsets, two `#`
comment header lines documenting the run:

```text
# vmaf-roi qpfile (x265, --qpfile-style)
# frame=0 ctu=64 cols=30 rows=17 strength=6.000
0 1 2 3 ...
...
```

Feed it to x265 via `--qpfile`:

```bash
x265 --input-res 1920x1080 --fps 30 \
     --qpfile vmaf_roi_frame_0.txt \
     -o out.h265 input.yuv
```

### SVT-AV1 (`--encoder svt-av1`)

Raw binary: `int8_t` per CTU, row-major, no header. Length is exactly
`cols * rows` bytes. Pass via SVT-AV1's ROI map input:

```bash
SvtAv1EncApp -i input.yuv -w 1920 -h 1080 \
     --roi-map-file vmaf_roi_frame_0.bin \
     -b out.ivf
```

## Examples

### One-shot for frame 42 with the default placeholder

```bash
vmaf-roi --reference clip.yuv --width 1920 --height 1080 \
         --frame 42 --output frame_42.qp \
         --encoder x265 --ctu-size 64 --strength 6.0
```

### With a real saliency model

```bash
vmaf-roi --reference clip.yuv --width 1920 --height 1080 \
         --frame 42 --output frame_42.qp \
         --saliency-model model/tiny/mobilesal.onnx \
         --encoder x265 --strength 8.0
```

### 10-bit planar input

```bash
vmaf-roi --reference hdr_clip_420p10le.yuv --width 3840 --height 2160 \
         --frame 42 --pixel_format 420 --bitdepth 10 \
         --output frame_42.qp --encoder x265 \
         --saliency-model model/tiny/mobilesal.onnx
```

### Per-frame loop (shell-driver pattern)

```bash
for f in $(seq 0 99); do
    vmaf-roi --reference clip.yuv --width 1920 --height 1080 \
             --frame "$f" --output "qp/frame_${f}.qp" \
             --saliency-model model/tiny/mobilesal.onnx
done
```

(A built-in batch mode is on the roadmap; see roadmap §2.3.)

## Caveats

- **Placeholder is for smoke testing only.** Without `--saliency-model`
  the tool emits a center-weighted radial map that has zero perceptual
  validity. Do not drive a real encode from it.
- **High-bit-depth input is luma8-normalised.** `--bitdepth 10|12|16`
  accepts little-endian 16-bit planar YUV, skips chroma using the
  selected `--pixel_format`, and downscales luma to the saliency
  model's existing 8-bit input contract. The ROI sidecar itself remains
  per-CTU QP offsets, not a high-bit-depth image output.
- **Single frame per invocation.** Wave 1 keeps the sidecar one-frame at
  a time so callers can reuse it from any encoder driver. A streaming
  variant is a follow-up.

## See also

- [ADR-0247](../adr/0247-vmaf-roi-tool.md) — the decision record (sidecar format, encoder coverage, signal blend).
- [`docs/ai/roadmap.md` §2.3](../ai/roadmap.md) — Wave 1 saliency surface.
- [`docs/usage/cli.md`](cli.md) — index of fork CLIs.

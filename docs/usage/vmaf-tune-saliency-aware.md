<!-- markdownlint-disable MD060 -->
# `vmaf-tune recommend-saliency --saliency-aware`

Use `vmaf-tune recommend-saliency --saliency-aware` to make one encode
that spends more bits on salient regions (faces, focal subjects, action)
and fewer on the background. It runs the fork-trained `saliency_student_v1`
ONNX model ([ADR-0286](../adr/0286-saliency-student-fork-trained-on-duts.md),
[ADR-0293](../adr/0293-vmaf-tune-saliency-aware.md)) over the source,
turns the result into the encoder's own ROI or QP control, and encodes
through the normal codec-adapter path. The code is
`tools/vmaf-tune/src/vmaftune/saliency.py`, wired in `cli.py`.

The command is a one-shot encode at `--crf` (or the adapter default). It
does not search for a target VMAF; that is the job of `recommend`,
`compare` or your own bisect loop.

## Quick start

```shell
vmaf-tune recommend-saliency \
    --src ref.yuv \
    --width 1920 --height 1080 --pix-fmt yuv420p \
    --framerate 24 --duration-frames 240 \
    --encoder libx264 \
    --preset medium --crf 23 \
    --saliency-aware \
    --output roi.mp4
```

Without `--saliency-aware` the command makes a plain encode. The flag
set requires `--src`, `--width`, `--height`, `--duration-frames` and
`--output`. The source must be raw YUV.

On success the command prints a JSON result to stdout with the encoder,
preset, CRF, output path, size, timing, versions, the aggregator and the
`exit_status`; the process exit code is the encode's exit status. If
`--output` ends in `.json`, the video goes to `<stem>_encoded.mp4`, the
JSON is written to the `.json` path, and that path is printed.

## Flags

| Flag | Default | Meaning |
|---|---|---|
| `--src PATH` | required | Raw YUV reference. |
| `--width`, `--height` | required | Source geometry. Any size works: the saliency model sees the frame zero-padded to a multiple of 32 and the map is cropped back, so heights such as 324 need no preparation. |
| `--pix-fmt` | `yuv420p` | Source pixel format. |
| `--framerate` | `24.0` | Source frame rate. |
| `--duration-frames N` | required | Frame count to compute saliency over, typically the whole clip. |
| `--encoder` | `libx264` | Codec adapter; ROI is supported for the five encoders below. |
| `--preset` | `medium` | Codec preset. |
| `--crf N` | adapter default | Base quality before ROI offsets (23 for `libx264`, 35 for `libsvtav1`). |
| `--saliency-aware` | off | Turn the saliency bias on. Without it the encode is plain. |
| `--saliency-offset` | `-4` | QP delta at peak saliency; clamped to +-12. |
| `--saliency-model PATH` | shipped model | Override the saliency ONNX path. |
| `--saliency-aggregator` | `mean` | Temporal reducer: `mean`, `ema`, `max` or `motion-weighted`. |
| `--saliency-ema-alpha` | `0.6` | Current-frame weight for `ema`, in `(0, 1]`. |
| `--saliency-fallback-plain` | off | Accept a plain encode for an encoder without ROI support. |
| `--ffmpeg-bin` | `ffmpeg` | FFmpeg binary. |
| `--output PATH` | required | Encoded output, or a `.json` report path. |

The shipped default model is documented in
[`saliency_student_v1.md`](../ai/models/saliency_student_v1.md). Do not
point `--saliency-model` at the `mobilesal_placeholder_v0` stub or the
radial fallback inside `vmaf-roi`; both are smoke-test stubs with no
perceptual benefit.

## How it works

1. `compute_saliency_map()` samples frames from the source (8 by default),
   runs them through `saliency_student_v1.onnx` and reduces the per-frame
   outputs to one mask in `[0, 1]`. The input is ImageNet-normalised RGB
   (NCHW `[1, 3, H, W]`) derived from BT.709-limited `yuv420p`, with
   chroma upsampled to luma size by nearest neighbour.
2. `saliency_to_qp_map()` maps the mask linearly to per-pixel QP deltas.
   `--saliency-offset` is the delta at peak saliency (negative means
   better quality there), the background gets the symmetric positive
   delta, and a neutral mask value of 0.5 gives 0. The result is clamped
   to `[-12, +12]`, the same convention as the
   [`vmaf-roi`](vmaf-roi.md) sidecar. `--crf` is the baseline; the deltas
   apply on top of the encoder's own rate control.
3. The map is dispatched to the encoder's ROI channel (table below). Each
   channel reduces it to its native granularity before it writes a
   sidecar file or builds the argv slice.
4. The augmented `extra_params` (qpfile path, zones string or ROI map
   path) are appended to the FFmpeg command and the normal encode runs.

The qpfile or ROI file is generated internally, in a temporary location by
default. To produce one yourself from Python, use `write_x264_qpfile()`
(see the helpers below); there is no standalone `python -m
vmaftune.saliency` command.

## Supported encoders

| Encoder | ROI channel | Granularity | argv slot |
|---|---|---|---|
| `libx264` | patched FFmpeg `-qpfile` ROI bridge (per-macroblock offsets through x264 `quant_offsets`; needs `aq-mode` other than 0) | 16x16 luma macroblock | `-qpfile ...` |
| `libaom-av1` | patched FFmpeg `-qpfile` ROI bridge | 16x16 macroblock mapped onto libaom mode-info cells | `-qpfile ...` |
| `libx265` | `--zones` QP delta | whole-clip spatial mean | `-x265-params zones=0,N,q=<delta>` |
| `libsvtav1` | `--qp-file` offset map (SVT-AV1 v1.7 or newer) | 64x64 super-block | `-svtav1-params qp-file=...` |
| `libvvenc` | `ROIFile` CSV (VVenC v1.14.0 or newer) | 64x64 CTU | `-vvenc-params ROIFile=...` |

See [ADR-0293](../adr/0293-vmaf-tune-saliency-aware.md) for the x264
baseline and [ADR-0414](../adr/0414-saliency-roi-x265-svtav1-vvenc.md)
for x265, SVT-AV1 and VVenC. The `libaom-av1` row uses the shared
`-qpfile` bridge documented in
[`vmaf-tune-ffmpeg.md`](vmaf-tune-ffmpeg.md#libaom-av1-full-roi-bridge).

Per-adapter helpers in `vmaftune.saliency`:

- `write_x265_zones_arg(block_offsets, duration_frames)` returns the zones
  string.
- `write_x264_qpfile(block_offsets, out_path, duration_frames)` writes the
  x264 and libaom qpfile.
- `write_svtav1_qpoffset_map(block_offsets, out_path, duration_frames)`
  returns the path.
- `write_vvenc_roi_csv(block_offsets, out_path, duration_frames)` returns
  the path.

## Temporal aggregation

`--saliency-aggregator` sets how the sampled per-frame masks become the
single ROI pattern used for the encode:

| Aggregator | Behaviour | Use when |
|---|---|---|
| `mean` | Per-pixel arithmetic mean over the sampled frames. The historical behaviour. | Default, stable clips, baseline comparisons. |
| `ema` | Exponential moving average; `--saliency-ema-alpha` is the current-frame weight, older frames decay geometrically. | Scene changes or motion bursts make the latest frames more representative. |
| `max` | Per-pixel maximum over the sampled masks. | Missing a briefly salient object is worse than over-protecting background, for example sports or highlights. |
| `motion-weighted` | Weighted mean; each frame is weighted by its luma difference from the previous sampled frame. | Motion-heavy clips where changing frames should dominate. |

All four use the same `saliency_student_v1` weights and the same QP
mapping, so the choice changes neither the model contract nor the encoder
sidecar format. The default matches the behaviour before
[ADR-0396](../adr/0396-video-saliency-extension.md).

## Trade-offs

Indicative figures:

| Axis | Direction |
|---|---|
| Bitrate at the same VMAF | 10 to 20 % lower for content with strong attention focus (faces, action, sport); little change for uniform backgrounds. |
| Encode time | About 5 % more (saliency inference plus the block reduce; model time per frame is sub-millisecond on CPU at SD or HD). |
| Decode time | Unchanged; the bitstream is standard-compliant for all five encoders. |
| VMAF | Unchanged at the clip-mean level, concentrated where the eye looks. |

## Fallbacks and exit codes

| Situation | Result |
|---|---|
| `onnxruntime` missing or the model cannot be loaded | A warning is logged and the command makes a plain encode. |
| Encoder has no ROI dispatch (for example `h264_nvenc`, `hevc_nvenc`, `libvpx-vp9`) | Exit code 2 with a message listing the supported encoders. |
| Same, with `--saliency-fallback-plain` or `VMAFTUNE_SALIENCY_FALLBACK_OK=1` | A plain encode runs and an ERROR is logged. |
| Encode fails | The exit code is the encode's exit status. |

The hard fail for an unsupported encoder follows the "requested but
unavailable fails" posture of ADR-0498 and ADR-0546. The environment
variable and the flag are equivalent.

## Caveats

- **One aggregate mask.** Saliency is reduced across the sampled frames
  and one delta pattern applies to the whole clip. Per-frame ROI is on
  the roadmap.
- **x265 zones carry a spatial mean only.** x265's `--zones` has temporal
  but no per-block spatial granularity, so the zone holds the mean QP
  delta of all blocks. Per-block x265 ROI needs a future x265 qpfile port,
  whose format differs from x264's.
- **libaom segment quantisation.** The FFmpeg bridge maps 16x16 deltas
  onto libaom's mode-info grid with at most eight segment QPs, so very
  fine deltas snap to the nearest segment.
- **SVT-AV1 and VVenC use 64x64 blocks.** Both document 64x64 as the ROI
  unit, so the mask is reduced with `reduce_qp_map_to_blocks(qp_map,
  block=64)` before writing.

## Reproducer

The tests mock the ONNX session and the encode runner, so they run
without FFmpeg or onnxruntime:

```shell
pytest tools/vmaf-tune/tests/test_saliency.py \
       tools/vmaf-tune/tests/test_saliency_roi_adapters.py \
       tools/vmaf-tune/tests/test_saliency_roi_codec.py -v
```

## History

- ADR-0293 wired the model into `vmaf-tune` as bucket #2 of the
  [PR #354](https://github.com/VMAFx/vmafx/pull/354) audit, with `libx264`
  as the baseline encoder. ADR-0414 added x265, SVT-AV1 and VVenC, and
  ADR-0396 added the temporal aggregators.

## See also

- [`vmaf-tune.md`](vmaf-tune.md): the base tool.
- [`vmaf-tune-codec-adapters.md`](vmaf-tune-codec-adapters.md): the
  adapters and which flag marks ROI support.
- [`vmaf-tune-ffmpeg.md`](vmaf-tune-ffmpeg.md): the FFmpeg `-qpfile`
  patch.
- [`vmaf-roi-score.md`](vmaf-roi-score.md): saliency-weighted scoring.
- [`vmaf-roi.md`](vmaf-roi.md): the C sidecar with the same QP
  convention.
- [ADR-0293](../adr/0293-vmaf-tune-saliency-aware.md): the design
  decision.

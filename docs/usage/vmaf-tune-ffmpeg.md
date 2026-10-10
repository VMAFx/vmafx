<!-- markdownlint-disable MD060 -->
# Using vmaf-tune with FFmpeg's encoder-side hooks

Four FFmpeg-side hooks from the `ffmpeg-patches/` series let you use
`vmaf-tune`'s results from a plain `ffmpeg` command line. This page shows
how to build the patched FFmpeg and how each hook fits a vmaf-tune
operating mode.

| Hook | Patch | What it does | vmaf-tune mode |
|---|---|---|---|
| `-qpfile <path>` | 0007 | Per-block QP offsets for `libx264`, `libsvtav1`, `libaom-av1`. | [Saliency-aware encoding](vmaf-tune-saliency-aware.md) |
| `-vf libvmaf_tune` | 0008 | Scores a pair and logs a recommended CRF. | CRF recommendation |
| `-pass-autotune` | 0009 | Advisory flag for external 2-pass orchestration. | 2-pass autotuning |
| `-vmaf-profile <path>` | 0015 | Advisory hand-off to `vmaf-tune encode-profile`. | Report-profile encodes |

## Prerequisites

Apply the fork's patches to a clean `n9.0.2` FFmpeg checkout (the
maintained stable release, verified on 2026-09-20).

1. Check out the release:

    ```bash
    cd /path/to/ffmpeg && git checkout n9.0.2
    ```

2. Apply the whole series in order:

    ```bash
    while IFS= read -r patch; do
        case "$patch" in ""|\#*) continue ;; esac
        git am --3way "/path/to/vmaf/ffmpeg-patches/$patch" || exit 1
    done < /path/to/vmaf/ffmpeg-patches/series.txt
    ```

3. Configure and build:

    ```bash
    ./configure --enable-libvmaf --enable-libx264 --enable-libsvtav1 --enable-libaom --enable-gpl
    make -j$(nproc)
    ```

Confirm that the new options are recognised:

```bash
./ffmpeg -h encoder=libx264   2>&1 | grep -i qpfile
./ffmpeg -h encoder=libsvtav1 2>&1 | grep -i qpfile
./ffmpeg -h encoder=libaom-av1 2>&1 | grep -i qpfile
./ffmpeg -h filter=libvmaf_tune
./ffmpeg -h | grep -i pass-autotune
./ffmpeg -h | grep -i vmaf-profile
```

## Hook 1: `-qpfile <path>` (patch 0007)

The `-qpfile` option on `libx264`, `libsvtav1` and `libaom-av1` reads the
qpfile format that vmaf-tune's saliency module
(`tools/vmaf-tune/src/vmaftune/saliency.py`) writes:

```text
<frame_idx> <I|P|B> <baseline_qp>
<delta_0_0> <delta_0_1> ... <delta_0_(bw-1)>
<delta_1_0> <delta_1_1> ... <delta_1_(bw-1)>
...
```

Each `delta` is a per-block QP offset clamped to `[-12, +12]`.

`<frame_idx>` is the 0-based coded-frame ordinal, the same index
`saliency.py` walks with `range(duration_frames)`. It is not a
presentation timestamp. The ROI adapters match each record by counting
the frames fed to the encoder and never compare against `AVFrame->pts`,
which is in stream `time_base` units and would not line up. When the
qpfile is shorter than the stream, the last record is reused for every
remaining frame, mirroring `saliency.py`'s "one mask, all frames"
fallback.

A shared parser, `libavcodec/qpfile_parser.{c,h}`, reads the format once.
Each encoder adapter then passes it to its native ROI or QP-offset API.

### Producing a qpfile

You usually do not need to write one. `vmaf-tune recommend-saliency`
builds the qpfile itself and passes it to the encoder (see
[saliency-aware encoding](vmaf-tune-saliency-aware.md)):

```bash
vmaf-tune recommend-saliency --src clip.yuv --width 1920 --height 1080 \
    --duration-frames 240 --saliency-aware --output out.mp4
```

To get the file for use with the patched `ffmpeg` directly, use the
Python helpers. `python -m vmaftune.saliency` is not a command: the
module has no entry point.

```python
from pathlib import Path

from vmaftune.saliency import (
    compute_saliency_map, reduce_qp_map_to_blocks,
    saliency_to_qp_map, write_x264_qpfile,
)

mask = compute_saliency_map(Path("clip.yuv"), 1920, 1080)
qp_map = saliency_to_qp_map(mask, baseline_qp=23, foreground_offset=-4)
write_x264_qpfile(reduce_qp_map_to_blocks(qp_map, block=16),
                  Path("clip.qpfile.txt"), duration_frames=240)
```

### libx264: fully wired

```bash
ffmpeg -f rawvideo -s 1920x1080 -pix_fmt yuv420p -i clip.yuv \
    -c:v libx264 -crf 23 -qpfile clip.qpfile.txt clip.mp4
```

libx264 has no `qpfile` parameter (the x264 command line reads the file
itself), so `-x264-params qpfile=...` is refused by libx264 and FFmpeg only
warns `Error parsing option`. The patch reads the file in the encoder wrapper
and gives each input frame's per-macroblock deltas to x264 as `quant_offsets`,
the channel FFmpeg's region-of-interest side data uses. x264 adds them to
its adaptive quantization, so `aq-mode` must not be 0 (the `ultrafast`
preset sets it to 0; the encoder then fails to open and says so), and the file's
block grid must be the video's macroblock grid. The per-frame type and
baseline QP of a record are not used: only the deltas.

### libsvtav1: full ROI bridge (SVT-AV1 1.6.0 or newer)

```bash
ffmpeg -f rawvideo ... -c:v libsvtav1 -crf 32 -qpfile clip.qpfile.txt clip.av1
```

The adapter loads the qpfile, sets `enable_roi_map` and attaches a
per-frame `ROI_MAP_EVENT` priv-data node to each picture sent to SVT-AV1:

```text
[libsvtav1 @ 0x...] libsvtav1: qpfile=clip.qpfile.txt loaded
(frames=240, 120x68 qpfile blocks -> 30x17 SB ROI grid); ROI bridge enabled.
```

It reduces the per-16x16 macroblock offsets from `saliency.py` to
SVT-AV1's per-64x64 superblock `b64_seg_map`. For each superblock it
averages the four overlapping macroblocks and snaps the result to at most
8 segment QPs, binning uniformly when the value span exceeds the segment
budget.

!!! note "ROIs smaller than 64x64 are averaged"
    Regions smaller than 64x64 pixels are averaged with their
    surroundings. That suits the object-sized ROIs of the
    `saliency_student_v1` model (faces, focal subjects).

On a release older than 1.6.0, which lacks the `enable_roi_map` flag and
the `ROI_MAP_EVENT` type, the adapter logs and encodes without ROI bias:

```text
[libsvtav1 @ 0x...] libsvtav1: qpfile=clip.qpfile.txt parsed but
SVT-AV1 < 1.6.0 lacks the ROI ABI; encoding without ROI bias.
Upgrade SVT-AV1 to activate the bridge (ADR-0312).
```

### libaom-av1: full ROI bridge

```bash
ffmpeg -f rawvideo ... -c:v libaom-av1 -crf 32 -qpfile clip.qpfile.txt clip.av1
```

The adapter loads the qpfile and allocates a per-mode-info-cell segment-id
map. The map is sized at libaom's mode-info grid, which is each dimension
aligned up to a multiple of 8 px (`ALIGN_POWER_OF_TWO(dim, 8)`) and
divided by 4. On every encoded frame it calls
`aom_codec_control(AOME_SET_ROI_MAP, ...)` with up to 8 segment QPs:

```text
[libaom-av1 @ 0x...] libaom: qpfile=clip.qpfile.txt loaded
(frames=240, 120x68 qpfile blocks -> 480x272 mi grid); ROI bridge enabled.
```

The adapter expands each per-16x16 offset into a 4x4 block of mode-info
cells, because libaom's grid is 4x4 luma pixels (`MI_SIZE` in
`av1/common/enums.h`).

For each frame it samples the offset range and picks at most 8 distinct
segment QPs. If the span fits in
`AOM_MAX_SEGMENTS` (8), every distinct value gets its own segment.
Otherwise the span is binned uniformly across 8 segments and each
macroblock rounds to the nearest. libaom copies the segment map and the
`delta_q[]` table on every control call (`av1_set_roi_map` in
`av1/encoder/encoder.c`), so the same buffer is reused across frames.

!!! note "8-segment limit"
    The limit is a quantisation step: nearby QP offsets round together.

## Hook 2: `-vf libvmaf_tune` (patch 0008)

The filter takes two video inputs, runs alongside a 1-pass encode and
emits a recommended CRF for the next pass:

```bash
ffmpeg -i input.mp4 -i reference.mp4 \
    -lavfi "[0:v][1:v]libvmaf_tune=recommend_target_vmaf=92:recommend_crf_min=18:recommend_crf_max=40" \
    -f null -
```

At the end of the run the filter logs:

```text
[Parsed_libvmaf_tune_0 @ 0x...] recommended_crf=24.3 (target_vmaf=92.0, observed_vmaf=93.6, n_frames=240)
```

!!! note "FFmpeg version"
    Patch 0008 uses the post-n7 libavfilter API (`ff_filter_link()` for
    per-link metadata such as `frame_rate`), so it needs FFmpeg `n7.0` or
    newer; it does not compile against `n6.x`. The series targets
    `n9.0.2`, so the prerequisites above satisfy this.

### Options

| Option | Default | Notes |
|---|---|---|
| `model` | `version=vmaf_v0.6.1` | libvmaf model spec; also accepts `path=...`. |
| `feature` | none | Optional `:`-separated feature spec. |
| `n_threads` | `0` | Worker threads (0 = libvmaf default). |
| `recommend_target_vmaf` | `95.0` | Target score the recommendation aims for. |
| `recommend_crf_min` | `18.0` | Lower CRF bound considered. |
| `recommend_crf_max` | `51.0` | Upper CRF bound considered. |
| `recommend_passes` | `1` | Probe-pass count (advisory; ignored in the single-pass implementation). |

The filter's default model is `vmaf_v0.6.1`. The Python `vmaf-tune`
tool defaults to `vmaf_v1.0.16_3d0h`, so the two recommendations can
differ unless you set the same model on both sides.

### Scoring and the recommendation

The filter runs real libvmaf scoring in-process. Each (main, ref) frame
pair is queued with `vmaf_read_pictures()`. At `uninit()` the filter
flushes and calls `vmaf_score_pooled(VMAF_POOL_METHOD_MEAN)`, so the
`observed_vmaf` in the final log line is the real pooled score.

The CRF recommendation is a piece-wise linear projection from the
observed VMAF onto `[recommend_crf_min, recommend_crf_max]`. The slope is
about 0.4 CRF per VMAF point near the 90 to 96 sweet spot, calibrated
roughly against libx264 `medium`. Per-clip calibration lives in the
Python tool: `tools/vmaf-tune/src/vmaftune/recommend.py` sweeps real
CRF-to-VMAF rows from a corpus instead of projecting from one
observation.

## Hook 3: `-pass-autotune` (patch 0009)

An advisory CLI flag for vmaf-tune-driven 2-pass encodes:

```bash
ffmpeg -i input.mp4 -c:v libx264 -pass-autotune -f null -
```

```text
[ffmpeg] -pass autotune: drive vmaf-tune externally; pass 1 frames
will be available for analysis. See docs/usage/vmaf-tune-ffmpeg.md.
```

The flag is glue only. FFmpeg behaves like a normal 1-pass encode and
prints the advisory line. Real 2-pass orchestration (probe pass,
recommend, final pass) lives in `tools/vmaf-tune/src/vmaftune/recommend.py`
and in [`--two-pass`](vmaf-tune-multipass.md). The flag exists so shell
scripts that call `ffmpeg` directly can signal the intent without
inventing their own log conventions.

## Hook 4: `-vmaf-profile <path>` (patch 0015)

`vmaf-tune compare --format html|both` and `vmaf-tune report` embed a
versioned `encoder_profile` payload in the generated report. The Python
orchestrator consumes it, because it holds codec-adapter defaults,
target-VMAF selection rules and schema-version handling that should not
be duplicated inside FFmpeg. The FFmpeg-side hook is therefore an
advisory hand-off:

```bash
ffmpeg -i input.mp4 -c:v libsvtav1 -vmaf-profile sweep_profile.html -f null -
```

```text
[ffmpeg] -vmaf-profile sweep_profile.html: encode with
`vmaf-tune encode-profile --profile sweep_profile.html --src INPUT --output OUTPUT`
(add --codec / --target-vmaf to select one recommendation). See
docs/usage/vmaf-tune-ffmpeg.md.
```

Run the encode through the profile reader. Inspect the exact FFmpeg argv
with `--dry-run` first:

```bash
vmaf-tune encode-profile \
    --profile sweep_profile.html \
    --src input.mp4 \
    --codec libsvtav1 \
    --target-vmaf 96 \
    --output out.mkv \
    --dry-run
```

Then run it without `--dry-run`:

```bash
vmaf-tune encode-profile \
    --profile sweep_profile.html \
    --src input.mp4 \
    --codec libsvtav1 \
    --target-vmaf 96 \
    --output out.mkv
```

The tool reads raw JSON, HTML or Markdown reports, picks one row with
`--codec`, `--target-vmaf` or `--recommendation-index`, and uses the same
codec-adapter registry as `vmaf-tune compare`. It does not encode every
codec or every ladder rung unless you script that loop yourself.

## End-to-end recipe: saliency-aware libx264 encode

1. Make the saliency-aware encode with vmaf-tune. The qpfile is generated
   and used internally:

    ```bash
    vmaf-tune recommend-saliency \
        --src ref.yuv --width 1920 --height 1080 --duration-frames 240 \
        --encoder libx264 --preset medium --crf 23 \
        --saliency-aware --saliency-offset -4 --output out.mp4
    ```

    To encode with a qpfile you wrote yourself (see
    [Producing a qpfile](#producing-a-qpfile)), use the patched FFmpeg:

    ```bash
    ffmpeg -f rawvideo -s 1920x1080 -pix_fmt yuv420p -i ref.yuv \
        -c:v libx264 -preset medium -crf 23 -qpfile ref.qpfile.txt out.mp4
    ```

2. Score the result and read the recommended CRF:

    ```bash
    ffmpeg -i out.mp4 \
        -f rawvideo -s 1920x1080 -pix_fmt yuv420p -i ref.yuv \
        -lavfi "[0:v][1:v]libvmaf_tune=recommend_target_vmaf=95" \
        -f null - 2>&1 | grep recommended_crf
    ```

If step 2 reports a `recommended_crf` well away from 23, re-encode with
the suggested value.

## Troubleshooting

### `unknown option qpfile`

You are running unpatched FFmpeg. Re-apply the patch series and rebuild;
see [Prerequisites](#prerequisites).

### `libx264: cannot read qpfile ...`, `... needs adaptive quantization`, `... macroblocks`

The file does not exist or is not in the format shown at the start of Hook
1 (a header line per frame, then one row of block deltas per block row);
the preset or `-x264-params` sets `aq-mode=0` (`ultrafast` does); or the
file's block grid is not the video's macroblock grid (width and height
divided by 16, rounded up). The message names which. There is no validator
command.

### `libsvtav1: qpfile=... parsed but SVT-AV1 < 1.6.0 lacks the ROI ABI`

The ROI bridge needs SVT-AV1 1.6.0 or newer, which added the
`enable_roi_map` flag and the `ROI_MAP_EVENT` type. Upgrade SVT-AV1. In
the meantime `vmaf-tune recommend-saliency --encoder libsvtav1
--saliency-aware` drives SVT-AV1 through its own
`-svtav1-params qp-file=...` option instead.

## Other changes in the build: the shared FFmpeg fix series

Every VMAFx FFmpeg build applies the shared FFmpeg fix series before the
patches of this repository
([FFmpeg patch automation](../development/ffmpeg-patch-automation.md#shared-ffmpeg-fix-series)).
It is unrelated to vmaf-tune, and three of its patches change how FFmpeg
behaves:

- On a Vulkan device with a single queue family, a hardware frame is no longer
  released to `VK_QUEUE_FAMILY_IGNORED` by a filter or codec barrier
  (`libavutil/vulkan.c`); the frame stays owned by its queue family.
- `hevc_nvenc` with `udu_sei=1` no longer stops the encode with "Failed locking
  bitstream buffer: out of memory" when a picture's user data unregistered SEI
  is large: a payload that does not fit NVENC's 1024-byte limit for non-VCL
  NAL units is not written, with a warning.
- `h264_nvenc` and `hevc_nvenc` do not write a user data unregistered SEI that
  NVENC would write truncated and undecodable (payloads dominated by zero
  bytes); the first one is a warning.

Its diagnostics patch (number 0019 of this repository's series until
ADR-3143) makes failures exposed by the warning-clean `n9.0.2` build
observable instead of truncating data or continuing after an error:

- Malformed AAC SBR, RV60 block geometry, and WMA channel metadata return
  an invalid-data error. A file that previously reached undefined or
  out-of-bounds behavior may therefore stop decoding with an explicit
  error.
- The AAC encoder rejects an impossible psychoacoustic window count
  instead of indexing beyond its eight-window grouping state. This
  reports an internal encoder error; valid one-window and eight-window
  inputs are unchanged.
- RTSP setup returns an error when a Transport or SETUP header exceeds its
  bounded buffer, or when sending the SETUP command fails. Headers are
  never silently truncated.
- DASH/HLS and Smooth Streaming muxing propagates playlist, fragment,
  copy, rename, and overlong-path failures to the `ffmpeg` process.
  Automation should treat the non-zero exit as an incomplete output and
  retry only after correcting the filesystem, network, or path problem.
- The companion `ismindex` tool constructs output paths without a fixed
  truncation limit and returns non-zero when allocation, open, or
  manifest write/close fails. An empty `-output` value no longer reads
  before the string; it retains the current directory prefix.
- Matroska muxing rejects a non-finite, negative, or 100-hour-or-longer
  stream duration when writing its fixed-width `DURATION` tag; it no
  longer emits a truncated tag.

These checks do not change successful inputs or outputs. They make
formerly ignored failure states fail closed, so callers must inspect the
process exit status rather than relying only on the presence of an output
file.

## See also

- [ADR-0312](../adr/0312-ffmpeg-patches-vmaf-tune-integration.md):
  decision context.
- [Research-0084](../research/0084-ffmpeg-patch-vmaf-tune-integration-survey.md):
  survey of VQA-tool and FFmpeg integration patterns.
- [`vmaf-tune.md`](vmaf-tune.md): the base tool.
- [`vmaf-tune-saliency-aware.md`](vmaf-tune-saliency-aware.md): the
  saliency pipeline that produces qpfiles.
- [`vmaf-tune-multipass.md`](vmaf-tune-multipass.md): `--two-pass`.
- [`vmaf-tune-codec-adapters.md`](vmaf-tune-codec-adapters.md): the codec
  registry that `encode-profile` uses.
- [`tools/vmaf-tune/`](../../tools/vmaf-tune/): the harness that emits
  qpfiles and runs the recommend loop.
- [`ffmpeg-patches/`](../../ffmpeg-patches/): the patch series.

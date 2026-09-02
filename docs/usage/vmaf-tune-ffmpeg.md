# Using vmaf-tune with FFmpeg's encoder-side hooks

The `tools/vmaf-tune/` orchestrator drives encodes through FFmpeg.
Four FFmpeg-side hooks (added via the `ffmpeg-patches/` 0007–0009
series plus the 0015 profile hand-off patch) make that integration
first-class instead of out-of-band.

This page documents how each hook plugs into vmaf-tune's operating
modes: **saliency-aware encoding**, **CRF recommendation**,
**2-pass autotuning**, and **report-profile driven encodes**.

## Prerequisites

Apply the fork's FFmpeg patches against a clean `n9.0.1` checkout
(latest released FFmpeg 8.x.x tag verified on 2026-05-20):

```bash
cd /path/to/ffmpeg && git checkout n9.0.1
for p in /path/to/vmaf/ffmpeg-patches/000*-*.patch; do
    git am --3way "$p" || break
done
./configure --enable-libvmaf --enable-libx264 --enable-libsvtav1 --enable-libaom --enable-gpl
make -j$(nproc)
```

Confirm the new options are recognised:

```bash
./ffmpeg -h encoder=libx264   2>&1 | grep -i qpfile
./ffmpeg -h encoder=libsvtav1 2>&1 | grep -i qpfile
./ffmpeg -h encoder=libaom-av1 2>&1 | grep -i qpfile
./ffmpeg -h filter=libvmaf_tune
./ffmpeg -h | grep -i pass-autotune
./ffmpeg -h | grep -i vmaf-profile
```

## Hook 1: `-qpfile <path>` (patch 0007)

The new `-qpfile` AVOption on `libx264`, `libsvtav1`, and `libaom-av1`
consumes the qpfile format emitted by vmaf-tune's saliency module
(`tools/vmaf-tune/src/vmaftune/saliency.py`):

```text
<frame_idx> <I|P|B> <baseline_qp>
<delta_0_0> <delta_0_1> ... <delta_0_(bw-1)>
<delta_1_0> <delta_1_1> ... <delta_1_(bw-1)>
...
```

**Where each `delta` is a per-block QP offset clamped to `[-12, +12]`.**

`<frame_idx>` is the **0-based coded-frame ordinal** — the same index
`saliency.py` walks via `range(duration_frames)`, i.e. the position of
the frame in the encoder's input/coded order (frame 0, 1, 2, …). It is
**not** a presentation timestamp: the ROI adapters match each record by
counting frames fed to the encoder, never against `AVFrame->pts` (which
is in stream `time_base` units and would not line up). When the qpfile
is shorter than the stream, the last record is reused for all remaining
frames, mirroring `saliency.py`'s "one mask, all frames" fallback.

The shared parser at `libavcodec/qpfile_parser.{c,h}` reads this
format once, then each encoder adapter dispatches it to its native
ROI/QP-offset API.

### libx264 — fully wired

```bash
# 1. vmaf-tune emits a saliency-driven qpfile
python -m vmaftune.saliency --source clip.yuv --width 1920 --height 1080 \
    --output clip.qpfile.txt

# 2. ffmpeg consumes it directly
ffmpeg -f rawvideo -s 1920x1080 -pix_fmt yuv420p -i clip.yuv \
    -c:v libx264 -crf 23 -qpfile clip.qpfile.txt clip.mp4
```

x264 honours per-MB QP deltas natively (since r2390); the patch
forwards `-qpfile <path>` to `x264_param_parse(... "qpfile", path)`.

### libsvtav1 — full ROI bridge (SVT-AV1 ≥ 1.6.0)

```bash
ffmpeg -f rawvideo ... -c:v libsvtav1 -crf 32 -qpfile clip.qpfile.txt clip.av1
```

Today this loads the qpfile, sets `enable_roi_map`, and attaches a
per-frame `ROI_MAP_EVENT` priv-data node to each picture sent to
SVT-AV1:

```text
[libsvtav1 @ 0x…] libsvtav1: qpfile=clip.qpfile.txt loaded
(frames=240, 120x68 qpfile blocks → 30x17 SB ROI grid); ROI bridge enabled.
```

The adapter downsamples the per-16×16-MB qp_offsets emitted by
`saliency.py` to SVT-AV1's per-64×64-superblock `b64_seg_map` by
averaging the four MBs that overlap each SB and snapping to up to 8
segment QPs (uniform binning when the value span exceeds the
segment budget). The trade-off is that ROIs smaller than 64×64 px
get averaged with their surroundings — acceptable for the
`saliency_student_v1` model's object-sized ROIs (faces, focal
subjects), but if you need finer granularity, drive the encode
through `vmaf-tune corpus` instead, which uses SVT-AV1's existing
`-svtav1-params roi-map=…` plumbing.

The bridge is gated on the SVT-AV1 1.6.0+ ROI ABI (the
`enable_roi_map` flag and the `ROI_MAP_EVENT` priv-data type). On
older releases the adapter falls back to log-and-continue:

```text
[libsvtav1 @ 0x…] libsvtav1: qpfile=clip.qpfile.txt parsed but
SVT-AV1 < 1.6.0 lacks the ROI ABI; encoding without ROI bias.
Upgrade SVT-AV1 to activate the bridge (ADR-0312).
```

### libaom-av1 — full ROI bridge

```bash
ffmpeg -f rawvideo ... -c:v libaom-av1 -crf 32 -qpfile clip.qpfile.txt clip.av1
```

Today this loads the qpfile, allocates a per-mi-cell segment-id map
sized at libaom's mode-info grid (`ALIGN_POWER_OF_TWO(dim, 8) >> 2`,
i.e. each dimension aligned up to a multiple of 8 px and divided by
4), and on every encoded frame issues
`aom_codec_control(AOME_SET_ROI_MAP, ...)` with up to 8 segment QPs:

```text
[libaom-av1 @ 0x…] libaom: qpfile=clip.qpfile.txt loaded
(frames=240, 120x68 qpfile blocks -> 480x272 mi grid); ROI bridge enabled.
```

The adapter expands each per-16×16-MB qp_offset (emitted by
`saliency.py`) to a 4×4 block of mi cells, since libaom's mi grid is
at 4×4-luma-pixel granularity (`av1/common/enums.h::MI_SIZE`). For
each frame, the qp_offset value range is sampled and at most 8 distinct
segment QPs are picked: when the span fits within `AOM_MAX_SEGMENTS`
(== 8) each distinct value becomes its own segment; otherwise the
span is uniformly binned across 8 segments and each MB rounds to its
nearest segment QP. libaom deep-copies the segment map and `delta_q[]`
table on every control call (per
`av1/encoder/encoder.c::av1_set_roi_map`), so the same buffer is
reused across frames.

Trade-off: the 8-segment limit is a quantisation step that rounds
nearby QP offsets together. For fine-grained per-MB control or
arbitrary segment counts, drive the encode through `vmaf-tune corpus`,
which uses libaom's lower-level rate-control plumbing instead.

## Hook 2: `-vf libvmaf_tune` (patch 0008)

> **FFmpeg version**: requires FFmpeg `n7.0` or newer. Patch 0008
> uses the post-n7 libavfilter API (`ff_filter_link()` accessor for
> per-link metadata such as `frame_rate`); building against `n6.x`
> trees fails to compile.

A new 2-input video filter that runs alongside a 1-pass encode and
emits a recommended CRF for the next pass:

```bash
ffmpeg -i input.mp4 -i reference.mp4 \
    -lavfi "[0:v][1:v]libvmaf_tune=recommend_target_vmaf=92:recommend_crf_min=18:recommend_crf_max=40" \
    -f null -
```

At the end of the run the filter logs:

```text
[Parsed_libvmaf_tune_0 @ 0x…] recommended_crf=24.3 (target_vmaf=92.0, observed_vmaf=93.6, n_frames=240)
```

### Options

| Option                  | Default               | Notes                                                     |
|-------------------------|-----------------------|-----------------------------------------------------------|
| `model`                 | `version=vmaf_v0.6.1` | libvmaf model spec (also accepts `path=…`).               |
| `feature`               | (none)                | Optional `:`-separated feature spec.                      |
| `n_threads`             | `0`                   | Worker threads (0 = libvmaf default).                     |
| `recommend_target_vmaf` | `95.0`                | Target score the recommendation aims for.                 |
| `recommend_crf_min`     | `18.0`                | Lower CRF bound considered.                               |
| `recommend_crf_max`     | `51.0`                | Upper CRF bound considered.                               |
| `recommend_passes`      | `1`                   | Probe-pass count (advisory; ignored in single-pass impl). |

### Scoring (full impl)

The filter runs real libvmaf scoring in-process: every (main, ref)
frame pair is queued via `vmaf_read_pictures()`; at uninit() the
filter flushes and calls `vmaf_score_pooled(VMAF_POOL_METHOD_MEAN)`
to extract the mean over the seen frames. The `observed_vmaf` field
in the final-line log is the real pooled score, not a placeholder.

The CRF recommendation is still a **piece-wise linear** projection
from the observed VMAF onto `[recommend_crf_min, recommend_crf_max]`
(slope ≈ 0.4 CRF / VMAF point near the 90–96 sweet spot, calibrated
roughly against libx264 medium-preset). Per-clip calibration data
that would replace this heuristic stays in the Python tool —
`tools/vmaf-tune/src/vmaftune/recommend.py` sweeps real CRF→VMAF
rows from a corpus rather than projecting from a single observation.

## Hook 3: `-pass-autotune` (patch 0009)

A new advisory CLI flag for vmaf-tune-driven 2-pass encodes:

```bash
ffmpeg -i input.mp4 -c:v libx264 -pass-autotune -f null -
```

```text
[ffmpeg] -pass autotune: drive vmaf-tune externally; pass 1 frames
will be available for analysis. See docs/usage/vmaf-tune-ffmpeg.md.
```

The flag is **glue only** — when set, FFmpeg behaves like a normal
1-pass encode and prints the advisory line. Real 2-pass orchestration
(probe pass → recommend → final pass) lives in
`tools/vmaf-tune/src/vmaftune/recommend.py`. The flag exists so
shell scripts that call ffmpeg directly can signal the user-visible
intent without inventing their own log conventions.

## Hook 4: `-vmaf-profile <path>` (patch 0015)

`vmaf-tune compare --format html|both` and `vmaf-tune report` embed a
versioned `encoder_profile` payload in the generated report. The
profile is consumed by the Python orchestrator because it contains
codec-adapter defaults, target-VMAF selection rules, and
schema-version handling that should not be duplicated inside FFmpeg.

The FFmpeg-side hook is therefore an advisory hand-off:

```bash
ffmpeg -i input.mp4 -c:v libsvtav1 -vmaf-profile sweep_profile.html -f null -
```

```text
[ffmpeg] -vmaf-profile sweep_profile.html: encode with
`vmaf-tune encode-profile --profile sweep_profile.html --src INPUT --output OUTPUT`
(add --codec / --target-vmaf to select one recommendation). See
docs/usage/vmaf-tune-ffmpeg.md.
```

Run the actual encode through the profile reader:

```bash
vmaf-tune encode-profile \
    --profile sweep_profile.html \
    --src input.mp4 \
    --codec libsvtav1 \
    --target-vmaf 96 \
    --output out.mkv \
    --dry-run

vmaf-tune encode-profile \
    --profile sweep_profile.html \
    --src input.mp4 \
    --codec libsvtav1 \
    --target-vmaf 96 \
    --output out.mkv
```

The tool reads raw JSON, HTML, or Markdown reports, chooses one row
with `--codec`, `--target-vmaf`, or `--recommendation-index`, and then
uses the same codec-adapter registry as `vmaf-tune compare`. It does
not encode every codec or every ladder rung unless the operator scripts
that loop explicitly.

## End-to-end recipe: saliency-aware libx264 encode

```bash
# 1. emit qpfile
python -m vmaftune.saliency \
    --source ref.yuv --width 1920 --height 1080 \
    --output ref.qpfile.txt --foreground-offset -4

# 2. encode with the qpfile
ffmpeg -f rawvideo -s 1920x1080 -pix_fmt yuv420p -i ref.yuv \
    -c:v libx264 -preset medium -crf 23 -qpfile ref.qpfile.txt out.mp4

# 3. score the result
ffmpeg -i ref.yuv -i out.mp4 \
    -lavfi "[0:v][1:v]libvmaf_tune=recommend_target_vmaf=95" \
    -f null - 2>&1 | grep recommended_crf
```

If step 3 reports `recommended_crf` significantly different from 23,
re-encode with the suggested value.

## Troubleshooting

### `unknown option qpfile`

You are running unpatched FFmpeg. Re-apply the patch series and
rebuild — see Prerequisites above.

### `libx264: failed to load qpfile=… (x264 ret=-1)`

Either the file does not exist, or its format does not match
x264's qpfile reader. Run `python -m vmaftune.saliency --validate
<path>` to round-trip the file through the parser.

### `libsvtav1: qpfile=… parsed but SVT-AV1 < 1.6.0 lacks the ROI ABI`

The full ROI bridge requires SVT-AV1 1.6.0 or newer (where the
`enable_roi_map` configuration flag and the `ROI_MAP_EVENT`
priv-data type were added). Upgrade your SVT-AV1 install, or in
the meantime use `vmaf-tune corpus --codec svtav1` for end-to-end
behaviour through SVT-AV1's existing `-svtav1-params roi-map=…`
plumbing.

## See also

- [ADR-0312](../adr/0312-ffmpeg-patches-vmaf-tune-integration.md) —
  decision context.
- [Research-0084](../research/0084-ffmpeg-patch-vmaf-tune-integration-survey.md)
  — survey of VQA-tool / FFmpeg integration patterns.
- [`tools/vmaf-tune/`](../../tools/vmaf-tune/) — the harness that emits
  qpfiles and runs the recommend loop.
- [`ffmpeg-patches/`](../../ffmpeg-patches/) — the patch series.

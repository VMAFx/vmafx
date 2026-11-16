<!-- markdownlint-disable MD013 MD046 MD060 -->
# What a VMAF number means

Most wrong VMAF numbers come from setup, not from the metric. This page says
what the score predicts, which viewing assumptions are baked into it, and the
pitfalls that bend it — each with the wrong and the right number where a
recorded pair exists, and a runnable command on the fixture pair
(`testdata/ref_576x324_48f.yuv` / `testdata/dis_576x324_48f.yuv`) where it
does not. The [chooser](chooser.md) picks the metric; the
[glossary](glossary.md) defines the terms.

## What the score is

VMAF predicts subjective quality on a 0–100 scale: 100 means the reference
itself, lower means visible damage. The score is computed **per frame** —
each frame's features (ADM, VIF, motion, …) are fused by the model — and the
per-frame scores are then collapsed into one number by
[pooling](glossary.md#pooling) (the default aggregate is the arithmetic
mean). A single-number comparison is only meaningful when both runs share the
model, its generation, the viewing condition and the pooling method.

## Viewing assumptions {#viewing-assumptions}

The classic training setup rescaled every clip to 1080 and showed it from
three times the screen height (3H) — the distance at which a 1080p display
resolves about 60 pixels per degree. Every model inherits that assumption:

- a **1080 video displayed from 3H** is what the score means;
- a **native 480-height pair** scored at its own size implies 3H = 6.75 ×
  its height, so artifacts hide and the score inflates — do not compare it
  with a 1080 run ([FAQ: the arithmetic](faq.md#q-when-computing-vmaf-on-low-resolution-videos-480-pixel-height-for-example-why-do-the-scores-look-so-high-even-when-there-are-visible-artifacts));
- the fix is [scaling](glossary.md#scaling): upsample to 1080 before scoring.

## Pitfalls {#pitfalls}

### Reversed inputs (FFmpeg pad order) {#reversed-inputs}

FFmpeg pad 0 is the distorted stream and pad 1 the reference — the opposite
of the CLI and Python surfaces. Reversing them does not fail; it scores the
reference against itself-as-distorted and inflates the number:

- **wrong** (reference fed as main): `83.782079`
- **right**: `76.667830` (matches the golden assertion)

measured on the Netflix pair, recorded in
[Research-0730](../research/0730-ffmpeg-libvmaf-smoke-20260527.md#bug-2-input-ordering-trap-libvmaf-filter-reverses-refdis-vs-python-runner)
and warned at the top of [FFmpeg: input ordering](../usage/ffmpeg.md#input-ordering).
The fork's `vmafx` filter logs the pad contract at initialization and pairs
frames by timestamp with pass-through for unpaired mains
([pairing](../usage/ffmpeg.md#pairing-the-two-inputs)).

### Model and generation mismatch {#model-mismatch}

The same pair under two models gives two different answers, and neither is
wrong:

- `vmaf_v0.6.1` on the Netflix pair: **76.667831**
- the fork default `vmaf_v1.0.16_3d0h` on the same pair: **82.816060**

([models: overview](../models/overview.md)). Pin `--model` when comparing
runs, or `--netflix-compat` to reproduce upstream numbers. NEG compounds
this: `vmaf_v0.6.1neg` exists on the v0.6.1 family only, so a NEG score and a
default-model score are never comparable ([NEG](glossary.md#neg)).

### HDR input with an SDR model {#hdr-input-with-an-sdr-model}

The classic models are trained on SDR content; PQ/HLG luma breaks their
assumptions, and blind metrics (BRISQUE, NIQE) are SDR-luma-trained
explicitly. Score HDR with the adapters — the `pu21` extractor's
`pu21_psnr` / `pu21_ssim` for perceptual PSNR/SSIM, and `delta_e_itp` for
colour difference — and state the transfer and matrix instead of trusting
defaults ([HDR](glossary.md#hdr), [ΔE-ITP options](../metrics/delta_e_itp.md)).
The fixture row below is a PQ→BT.709 round trip, not tone mapping; the adapter
commands above are the recommended HDR route.

Runnable on the fixture pair (SDR, so the numbers are meaningful) and on any
HDR pair you have — the fixture pair exercises the command shape:

```bash
vmaf -r testdata/ref_576x324_48f.yuv -d testdata/dis_576x324_48f.yuv \
    -w 576 -h 324 -p 420 -b 8 --feature pu21 --feature delta_e_itp \
    --json -o /dev/null
```

### Range (limited vs full) {#range-limited-vs-full}

A limited-range decode of full-range content (or the reverse) shifts every
luma value by the 16–235 offset: the metric then scores a level shift, which
reads as a score change that no encoder fix will remove. On this fixture the
mismatched score is **higher** (99.839253 against 94.692167), so a range error
can look like an improvement rather than a drop. State the range where the
surface allows it (`--pixel_format` covers the format; `delta_e_itp` carries
explicit range options), and keep both inputs decoded the same way
([range](glossary.md#range)). The fixture's source luma reaches 245, so "both
limited" in the table means the converted streams, not a strict 16–235 source.

### Frame alignment and offsets {#frame-alignment-and-offsets}

FFmpeg filters synchronize on timestamps, not frame counts: a missing
`setpts=PTS-STARTPTS` on a stream that does not start at PTS 0 pairs every
frame with the wrong reference. The stock `libvmaf` filter scores the tail of
a longer main stream against the last reference frame; the fork's `vmafx`
filter warns and passes unpaired mains instead
([pairing](../usage/ffmpeg.md#pairing-the-two-inputs)). Cut both inputs to
the same frames (`trim` / `-frames:v`) before the filter
([score exactly N frames](../usage/ffmpeg.md#score-exactly-n-frames)).

### Interlaced input {#interlaced-input}

VMAF's motion and temporal pooling assume progressive frames. Scoring an
interlaced stream field-by-field, or feeding the fields as full frames, mixes
two time instants and counts combing as motion and detail. Deinterlace both
inputs the same way (`bwdif` or `yadif`) before scoring, and never deinterlace
only one side — a progressive reference against interlaced distorted frames
measures the deinterlacer, not the encoder. The table's wrong score also
compares 48 reference frames with 24 woven frames, so part of that gap is
temporal misalignment; the right side deinterlaces both 24-frame streams.

### Self-comparison lands near 100 {#self-comparison-lands-near-100}

Scoring a video against itself lands at or near 100. On the fixture pair the
classic model scores 97.428025 on the first frame and pools to 97.732879
(min 97.428025, max 100.0), while the default `vmaf_v1.0.16_3d0h` pools to
99.995262 — 41 of its 48 frames are 100.0 and the other seven are below it.
A 100.0 is not a bug: `model/vmaf_v0.6.1.json` sets `score_clip` to
`[0.0, 100.0]`, and `core/src/predict.c` clamps every prediction into that
range. Neither the
upstream FAQ's ~98.7 nor a 100.000 on the same setup means the metric is
broken
([FAQ](faq.md#q-when-i-compare-a-video-with-itself-as-reference-i-expect-a-perfect-vmaf-score-of-100-but-i-see-987-is-this-a-bug)).

### Metric-implementation differences {#metric-implementation-differences}

The same metric name can mean slightly different computations in different
tools: the bundled SSIM applies the empirical downsampling step from the SSIM
paper and FFmpeg's `ssim` filter does not, and PSNR caps at 60 dB for 8-bit
input by design. Compare numbers produced by the same implementation
([FAQ: SSIM difference](faq.md#q-why-does-the-included-ssim-tool-produce-different-numbers-than-other-ssim-implementations),
[FAQ: PSNR caps](faq.md#q-why-are-psnr-values-capped-at-60-db-for-8-bit-inputs-72-db-for-10-bit-84-db-for-12-bit-and-108-db-for-16-bit)).

## Fixture checks {#fixture-checks}

All numbers below are pooled VMAF means from the CPU CLI on the 576x324
48-frame fixture pair (`testdata/ref_576x324_48f.yuv` /
`testdata/dis_576x324_48f.yuv`), with `vmaf_v0.6.1` unless the row names a
different model. "Wrong" is the setup the pitfall warns about; "right" is the
matched setup. For the Model row, "Wrong" means a different but equally valid
model, not a broken setup. The commands run from the repository root and write
their JSON reports into it; they also leave 13 raw streams, so run them in a
scratch checkout or use the closing `rm` line in the block.

| Pitfall | Wrong | Right |
|---|---|---|
| Reversed inputs | 94.63212 (dis passed as `-r`) | 94.32301 |
| Model / generation | 94.358152 (`vmaf_b_v0.6.3`, a second valid answer) | 94.32301 (`vmaf_v0.6.1`) |
| HDR input with an SDR model | 95.74606 (PQ luma into an SDR model) | 92.57015 (PQ→BT.709 round trip, not tone mapping) |
| Range | 99.839253 (limited ref against full dis; the score rises) | 94.692167 (both limited) |
| Frame alignment | 90.903612 (one-frame offset) | 94.32301 |
| Scaling | 94.32301 (native 576x324) | 84.777934 (upscaled to 1080) |
| Interlacing | 85.837692 (progressive ref, 24 interlaced frames) | 94.778674 (both deinterlaced, 24 frames) |

The six-decimal HDR values are rounded; FFmpeg n9.0.2 with zimg 3.0.6 printed
95.746061 and 92.570148. The 94.32301 in the Scaling row is the same native
score as the right column above — it is "wrong" only for a 1080 viewing
assumption, where the pair must be upscaled first.

```bash
# Reversed inputs: wrong order first, then the right order.
vmaf -r testdata/dis_576x324_48f.yuv -d testdata/ref_576x324_48f.yuv \
    -w 576 -h 324 -p 420 -b 8 -m path=model/vmaf_v0.6.1.json \
    --json -o reversed-wrong.json
vmaf -r testdata/ref_576x324_48f.yuv -d testdata/dis_576x324_48f.yuv \
    -w 576 -h 324 -p 420 -b 8 -m path=model/vmaf_v0.6.1.json \
    --json -o reversed-right.json

# Model / generation: the 0.6.3 model, then the 0.6.1 model.
vmaf -r testdata/ref_576x324_48f.yuv -d testdata/dis_576x324_48f.yuv \
    -w 576 -h 324 -p 420 -b 8 -m path=model/vmaf_b_v0.6.3.json \
    --json -o model-wrong.json
vmaf -r testdata/ref_576x324_48f.yuv -d testdata/dis_576x324_48f.yuv \
    -w 576 -h 324 -p 420 -b 8 -m path=model/vmaf_v0.6.1.json \
    --json -o model-right.json

# HDR with an SDR model: make a 10-bit PQ copy of the fixture, score it
# directly (wrong), then tone-map it back to SDR and score it (right).
ffmpeg -loglevel error -y -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 \
    -i testdata/ref_576x324_48f.yuv \
    -vf "zscale=pin=bt709:tin=bt709:min=bt709:p=bt2020:t=smpte2084:m=bt2020nc,format=yuv420p10le" \
    -pix_fmt yuv420p10le -f rawvideo ref_hdr.yuv
ffmpeg -loglevel error -y -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 \
    -i testdata/dis_576x324_48f.yuv \
    -vf "zscale=pin=bt709:tin=bt709:min=bt709:p=bt2020:t=smpte2084:m=bt2020nc,format=yuv420p10le" \
    -pix_fmt yuv420p10le -f rawvideo dis_hdr.yuv
vmaf -r ref_hdr.yuv -d dis_hdr.yuv -w 576 -h 324 -p 420 -b 10 \
    -m path=model/vmaf_v0.6.1.json --json -o hdr-wrong.json
ffmpeg -loglevel error -y -f rawvideo -pix_fmt yuv420p10le -s 576x324 -r 24 \
    -i ref_hdr.yuv \
    -vf "zscale=pin=bt2020:tin=smpte2084:min=bt2020nc:p=bt709:t=bt709:m=bt709,format=yuv420p" \
    -pix_fmt yuv420p -f rawvideo ref_sdr.yuv
ffmpeg -loglevel error -y -f rawvideo -pix_fmt yuv420p10le -s 576x324 -r 24 \
    -i dis_hdr.yuv \
    -vf "zscale=pin=bt2020:tin=smpte2084:min=bt2020nc:p=bt709:t=bt709:m=bt709,format=yuv420p" \
    -pix_fmt yuv420p -f rawvideo dis_sdr.yuv
vmaf -r ref_sdr.yuv -d dis_sdr.yuv -w 576 -h 324 -p 420 -b 8 \
    -m path=model/vmaf_v0.6.1.json --json -o hdr-right.json

# Range: convert both sides to limited once, then compare a limited ref
# against a full dis (wrong) and limited against limited (right).
ffmpeg -loglevel error -y -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 \
    -i testdata/ref_576x324_48f.yuv \
    -vf "scale=in_range=full:out_range=limited" -pix_fmt yuv420p \
    -f rawvideo ref_lim.yuv
ffmpeg -loglevel error -y -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 \
    -i testdata/dis_576x324_48f.yuv \
    -vf "scale=in_range=full:out_range=limited" -pix_fmt yuv420p \
    -f rawvideo dis_lim.yuv
vmaf -r ref_lim.yuv -d testdata/dis_576x324_48f.yuv -w 576 -h 324 -p 420 -b 8 \
    -m path=model/vmaf_v0.6.1.json --json -o range-wrong.json
vmaf -r ref_lim.yuv -d dis_lim.yuv -w 576 -h 324 -p 420 -b 8 \
    -m path=model/vmaf_v0.6.1.json --json -o range-right.json

# Frame alignment: drop the first distorted frame and repeat the last,
# then score the shifted stream against the reference.
python3 - <<'PY'
from pathlib import Path
frame = 576 * 324 * 3 // 2
dis = Path("testdata/dis_576x324_48f.yuv").read_bytes()
Path("dis_shift.yuv").write_bytes(dis[frame:] + dis[-frame:])
PY
vmaf -r testdata/ref_576x324_48f.yuv -d dis_shift.yuv -w 576 -h 324 -p 420 -b 8 \
    -m path=model/vmaf_v0.6.1.json --json -o offset-wrong.json
vmaf -r testdata/ref_576x324_48f.yuv -d testdata/dis_576x324_48f.yuv \
    -w 576 -h 324 -p 420 -b 8 -m path=model/vmaf_v0.6.1.json \
    --json -o offset-right.json

# Scaling: score the native pair, then the pair upscaled to 1080.
vmaf -r testdata/ref_576x324_48f.yuv -d testdata/dis_576x324_48f.yuv \
    -w 576 -h 324 -p 420 -b 8 -m path=model/vmaf_v0.6.1.json \
    --json -o scale-wrong.json
ffmpeg -loglevel error -y -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 \
    -i testdata/ref_576x324_48f.yuv -vf "scale=1920:1080:flags=lanczos" \
    -pix_fmt yuv420p -f rawvideo ref_1080.yuv
ffmpeg -loglevel error -y -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 \
    -i testdata/dis_576x324_48f.yuv -vf "scale=1920:1080:flags=lanczos" \
    -pix_fmt yuv420p -f rawvideo dis_1080.yuv
vmaf -r ref_1080.yuv -d dis_1080.yuv -w 1920 -h 1080 -p 420 -b 8 \
    -m path=model/vmaf_v0.6.1.json --json -o scale-right.json

# Interlacing: progressive ref against interlaced dis (wrong), then both
# sides interlaced and deinterlaced the same way (right).
ffmpeg -loglevel error -y -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 \
    -i testdata/ref_576x324_48f.yuv -vf interlace -pix_fmt yuv420p \
    -f rawvideo ref_int.yuv
ffmpeg -loglevel error -y -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 \
    -i testdata/dis_576x324_48f.yuv -vf interlace -pix_fmt yuv420p \
    -f rawvideo dis_int.yuv
vmaf -r testdata/ref_576x324_48f.yuv -d dis_int.yuv -w 576 -h 324 -p 420 -b 8 \
    -m path=model/vmaf_v0.6.1.json --json -o interlace-wrong.json
ffmpeg -loglevel error -y -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 \
    -i ref_int.yuv -vf bwdif -pix_fmt yuv420p -f rawvideo ref_int_deint.yuv
ffmpeg -loglevel error -y -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 \
    -i dis_int.yuv -vf bwdif -pix_fmt yuv420p -f rawvideo dis_deint.yuv
vmaf -r ref_int_deint.yuv -d dis_deint.yuv -w 576 -h 324 -p 420 -b 8 \
    -m path=model/vmaf_v0.6.1.json --json -o interlace-right.json

# Cleanup: the block leaves 13 raw streams and 14 JSON reports in the root.
rm -f ref_hdr.yuv dis_hdr.yuv ref_sdr.yuv dis_sdr.yuv ref_lim.yuv dis_lim.yuv \
    dis_shift.yuv ref_1080.yuv dis_1080.yuv ref_int.yuv dis_int.yuv \
    ref_int_deint.yuv dis_deint.yuv
rm -f reversed-wrong.json reversed-right.json model-wrong.json model-right.json \
    hdr-wrong.json hdr-right.json range-wrong.json range-right.json \
    offset-wrong.json offset-right.json scale-wrong.json scale-right.json \
    interlace-wrong.json interlace-right.json
```

The self-comparison row has no wrong/right pair: scoring a stream against
itself is the right setup and lands at 97.732879 (`vmaf_v0.6.1`) or 99.995262
(`vmaf_v1.0.16_3d0h`), as recorded above. The metric-implementation
difference has no fixture pair either — both tools are right for their own
implementation; compare only numbers from the same one.

## How every pitfall is held

The recorded wrong/right pairs above are quoted from the pages that measured
them (linked in each section); the docs check
(`scripts/docs/check_glossary_links.py`) holds every metric page to the
[glossary](glossary.md) and the chooser table to the same term list, so a
term that loses its entry — or a new metric page that uses one without
linking it — fails `make docs-fragments-check`.

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
explicitly. Score HDR with the adapters — `pu21_psnr` / `pu21_ssim` for
perceptual PSNR/SSIM, `delta_e_itp` for colour difference — and state the
transfer and matrix instead of trusting defaults
([HDR](glossary.md#hdr), [ΔE-ITP options](../metrics/delta_e_itp.md)).

Runnable on the fixture pair (SDR, so the numbers are meaningful) and on any
HDR pair you have — the fixture pair exercises the command shape:

```bash
vmaf -r testdata/ref_576x324_48f.yuv -d testdata/dis_576x324_48f.yuv \
    -w 576 -h 324 -p 420 -b 8 --feature pu21_psnr --feature delta_e_itp \
    --json -o /dev/null
```

### Range (limited vs full) {#range-limited-vs-full}

A limited-range decode of full-range content (or the reverse) shifts every
luma value by the 16–235 offset: the metric then scores a level shift, which
reads as a small quality drop that no encoder fix will remove. State the
range where the surface allows it (`--pixel_format` covers the format;
`delta_e_itp` carries explicit range options), and keep both inputs decoded
the same way ([range](glossary.md#range)).

### Frame alignment and offsets {#frame-alignment-and-offsets}

FFmpeg filters synchronize on timestamps, not frame counts: a missing
`setpts=PTS-STARTPTS` on a stream that does not start at PTS 0 pairs every
frame with the wrong reference. The stock `libvmaf` filter scores the tail of
a longer main stream against the last reference frame; the fork's `vmafx`
filter warns and passes unpaired mains instead
([pairing](../usage/ffmpeg.md#pairing-the-two-inputs)). Cut both inputs to
the same frames (`trim` / `-frames:v`) before the filter
([score exactly N frames](../usage/ffmpeg.md#score-exactly-n-frames)).

### Self-comparison is not 100 {#self-comparison-is-not-100}

Scoring a video against itself lands near but not exactly at 100 (~98.7 on
the classic model): ADM and VIF pass through a fit-polynomial stage without a
closed-form identity at zero distortion
([FAQ](faq.md#q-when-i-compare-a-video-with-itself-as-reference-i-expect-a-perfect-vmaf-score-of-100-but-i-see-987-is-this-a-bug)).
A 98.7 self-score is not a bug; a 100.000 on the same setup would be.

### Metric-implementation differences {#metric-implementation-differences}

The same metric name can mean slightly different computations in different
tools: the bundled SSIM applies the empirical downsampling step from the SSIM
paper and FFmpeg's `ssim` filter does not, and PSNR caps at 60 dB for 8-bit
input by design. Compare numbers produced by the same implementation
([FAQ: SSIM difference](faq.md#q-why-does-the-included-ssim-tool-produce-different-numbers-than-other-ssim-implementations),
[FAQ: PSNR caps](faq.md#q-why-are-psnr-values-capped-at-60-db-for-8-bit-inputs-72-db-for-10-bit-84-db-for-12-bit-and-108-db-for-16-bit)).

## How every pitfall is held

The recorded wrong/right pairs above are quoted from the pages that measured
them (linked in each section); the docs check
(`scripts/docs/check_glossary_links.py`) holds every metric page to the
[glossary](glossary.md) and the chooser table to the same term list, so a
term that loses its entry — or a new metric page that uses one without
linking it — fails `make docs-fragments-check`.

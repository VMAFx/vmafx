<!-- markdownlint-disable MD013 MD046 MD060 -->
# Glossary

One entry per term the metric pages use. Every entry names the flag or option
that controls the thing and links the page that documents it in depth. The
docs check (`scripts/docs/check_glossary_links.py`) holds the metric pages to
this list: a term used on a metric page must link back here, and a term the
checker knows about without an entry here fails the build.

The metric pages themselves are linked from the
[feature index](../metrics/features.md); the [FAQ](faq.md) and the
[concept guide](concepts.md) carry the pitfalls behind several of these
entries.

## Viewing condition {#viewing-condition}

The display size, resolution and distance the model was trained to predict
quality for. The classic model assumes a 1080p HDTV watched from three times
the screen height; every VMAF number is a prediction *at that condition*, and
scoring a pair at a different native resolution silently changes the implied
condition ([concepts: viewing assumptions](concepts.md#viewing-assumptions)).
Set per model: `vmaf_v1.0.16_3d0h` is 1080p at 3H, `vmaf_v1.0.16_5d0h` is the
phone condition, the `_2160` variants are 4K
([models: overview](../models/overview.md)).

## 3H {#3h}

Three times the screen height — the viewing distance of the classic training
setup, and the operating point the default model name encodes (`3d0h`). At 3H
a 1080p display resolves about 60 pixels per degree of visual angle.
Scoring a native 480-height pair at its own size implies 3H = 6.75 × its
height, which is why native-480 scores run high
([FAQ: low resolution](faq.md#q-when-computing-vmaf-on-low-resolution-videos-480-pixel-height-for-example-why-do-the-scores-look-so-high-even-when-there-are-visible-artifacts)).

## Phone model {#phone-model}

The score transform that predicts quality on a small phone screen instead of
a living-room display: a smaller screen viewed from a relatively larger
distance hides artifacts. On the v0 generation, enable it with the model
option `enable_transform=true` (Python: `--phone-model`); the v1 generation
ships it as a separate model, `vmaf_v1.0.16_5d0h` (5H distance)
([models: phone](../models/overview.md#predict-quality-on-a-cellular-phone-screen)).

## Default model {#default-model}

The model used when `--model` is not passed. The fork's default is
`vmaf_v1.0.16_3d0h` (1080p at 3H); upstream Netflix still defaults to
`vmaf_v0.6.1`, so the same command gives different numbers on upstream and on
this fork. `--netflix-compat` restores the upstream default together with the
`%.6f` precision ([models: overview](../models/overview.md),
[vmafx CLI](../usage/vmafx-cli.md)).

## Feature extractor {#feature-extractor}

A per-frame computation libvmaf runs as part of scoring, publishing one or
more named metrics into the report (`--feature adm`, `--feature vif`, …).
Models fuse a subset of the features into the VMAF score; the rest are
reported alongside ([feature index](../metrics/features.md)).

## Motion {#motion}

Temporal activity measured in the **reference** stream: the mean absolute
difference between consecutive Gaussian-blurred luma frames. Models use it to
weight distortion features — still content and moving content score
differently at the same distortion. `vmaf_v0.6.1still` forces it to 0 for
still-image scoring ([motion](../metrics/motion.md)).

## Pooling {#pooling}

The function that collapses per-frame scores into one number for a clip or a
window. The engine and every user surface agree on eight methods: `mean`
(the default aggregate), `harmonic_mean` (weights the worst frames more), the
order statistics `min`, `max`, `median`, `perc5`, `perc10` and `perc20`
([ADR-1188]). The `vmaf` CLI report always carries `min`, `max`, `mean` and
`harmonic_mean` side by side; the percentile methods are selectable through
the C API, the Python harness (`--pool`) and the FFmpeg `pool` option. Only
`mean` and `harmonic_mean` accept perceptual spatial weights.
([API: pooling methods](../api/lifecycle.md#vmafpoolingmethod),
[FAQ: aggregate bias](faq.md#q-why-is-the-aggregate-vmaf-score-sometimes-biased-toward-easy-content-upstream-netflixvmaf20)).

## NEG {#neg}

No Enhancement Gain. A model variant that penalises in-loop sharpening and
similar encoder tricks that inflate the standard score without helping the
viewer. Published for the v0.6.1 family only (`vmaf_v0.6.1neg`,
`vmaf_4k_v0.6.1neg`) — there is no NEG counterpart to any v1 model, so an
NEG score is not directly comparable with a default-model score: compare NEG
against NEG ([VMAF NEG](../metrics/vmaf-neg.md)).

## CTC {#ctc}

Common test conditions: a fixed model-and-feature preset so that encoder
comparisons follow a published recipe. The fork ships two as one-flag
presets — `--aom_ctc <version>` pins the AOM CTC set (the models stay on the
v0.6.1 family regardless of the build's default) and `--nflx_ctc v1.0` pins
the Netflix 4K set
([AOM CTC](../metrics/ctc/aom.md), [NFLX CTC](../metrics/ctc/nflx.md)).

## HDR {#hdr}

High dynamic range content (PQ or HLG transfer, typically BT.2020). The
classic models are trained on SDR content, so they misjudge HDR input; HDR
work needs the HDR adapters — `pu21_psnr`/`pu21_ssim` map luminance through a
contrast-sensitivity curve, and `delta_e_itp` measures colour difference in
ICtCp ([PU21](../metrics/pu21.md), [ΔE-ITP](../metrics/delta_e_itp.md),
[concepts: HDR input](concepts.md#hdr-input-with-an-sdr-model)).

## SDR {#sdr}

Standard dynamic range: the 8/10-bit BT.709-era content every classic model
was trained on. Blind metrics such as BRISQUE and NIQE are SDR-luma-trained
and break down on PQ/HLG luma
([concepts: HDR input](concepts.md#hdr-input-with-an-sdr-model)).

## PQ / HLG {#pqr-hlg}

The two HDR transfer functions: PQ (SMPTE ST 2084, absolute luminance, used
by HDR10) and HLG (BBC/NHK hybrid log-gamma, backward compatible with SDR
displays). Several extractors need the transfer stated, not guessed —
`delta_e_itp` defaults to PQ and takes `hdr` options
([ΔE-ITP](../metrics/delta_e_itp.md)).

## BT.709 / BT.2020 {#bt709-bt2020}

The SDR and HDR colour-space families (primaries + matrix): BT.709 for
SDR 1080p, BT.2020 for 4K/HDR. The matrix and range decide how YCbCr maps to
RGB before luma metrics run; a wrong matrix is a setup error, not a metric
error ([concepts: range](concepts.md#range-limited-vs-full)).

## Range {#range}

Limited (narrow, 16–235 for 8-bit luma) vs full (PC, 0–255) signal range. The
decode must match the encode; a mismatch shifts every luma value and shows up
as a small but real score change that has nothing to do with quality. Some
features state their assumption explicitly — `delta_e_itp` defaults to
limited-range PQ input ([ΔE-ITP](../metrics/delta_e_itp.md),
[concepts: range](concepts.md#range-limited-vs-full)).

## Frame alignment {#frame-alignment}

The pairing of each distorted frame with its reference frame. FFmpeg filters
pair by timestamp, not by frame count, and pad 0 is the distorted stream —
reversed pads silently score the wrong direction
([FFmpeg: input ordering](../usage/ffmpeg.md#input-ordering)). A frame offset
between the streams scores every frame against the wrong reference
([concepts: alignment](concepts.md#frame-alignment-and-offsets)).

## Scaling {#scaling}

Rescaling an input to the resolution the viewing condition expects. The
classic setup rescales everything to 1080 before scoring; a native-480 pair
scored at 480 implies a different viewing condition and inflates the score
([FAQ: low resolution](faq.md#q-when-computing-vmaf-on-low-resolution-videos-480-pixel-height-for-example-why-do-the-scores-look-so-high-even-when-there-are-visible-artifacts)).
The FFmpeg `vmafx` filter takes `target_scaling` for the same purpose.

## Interlacing {#interlacing}

Interlaced (field-based) material. The models assume progressive frames;
score interlaced content only after deinterlacing, and keep both inputs in
the same scan type — the engine does not deinterlace for you and no flag
compensates for it.

## Chroma subsampling {#chroma-subsampling}

4:2:0 / 4:2:2 / 4:4:4 — how much chroma resolution survives relative to
luma. The `--pixel_format` / `-p` flag tells the engine what it is reading;
luma-only metrics miss chroma damage that CIEDE2000 catches
([CIEDE2000](../metrics/ciede.md), [pixel formats](../usage/pixel-formats.md)).

## 10-bit {#10-bit}

Ten-bit sample depth. Internal metric computations are carried at 10-bit
(CAMBI converts every format to 10-bit before measuring), and the v1 models
prefer 10-bit input for banding accuracy. The `--bitdepth` / `-b` flag states
the input depth ([CAMBI](../metrics/cambi.md), [models: v1](../models/v1.md)).

## Confidence interval {#confidence-interval}

The 95% interval around a score that bootstrap models produce: score the pair
with `vmaf_b_v0.6.3` (the bootstrap model) and the report carries the interval
next to the score ([confidence interval](../metrics/confidence-interval.md)).

## Score transform {#score-transform}

The post-prediction nonlinear mapping applied by the phone model
(`enable_transform`) — same features, different output curve.
[score transform](#phone-model).

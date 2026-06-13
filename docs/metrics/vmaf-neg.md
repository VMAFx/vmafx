<!-- markdownlint-disable MD013 MD060 -->
# VMAF NEG (No Enhancement Gain)

VMAF NEG is a model variant that penalises encoder in-loop sharpening and similar
enhancements that inflate standard VMAF scores without delivering a genuine
improvement in perceived quality for the viewer.

## Background

Standard VMAF scores the similarity between a reference and a distorted video. It was
trained on human opinion scores for content encoded with typical compression artifacts.
One consequence: some encoders use in-loop sharpening (deblocking, deringing,
adaptive sharpening) to boost apparent edge contrast, which the VMAF feature set
interprets as closer to the reference even when the enhancement is perceptually
neutral or mildly annoying.

The NEG variant was trained to be resistant to such enhancements. "NEG" stands for
**No Enhancement Gain**: a sharpening filter that does not improve perceptual quality
produces no additional VMAF score. This makes NEG appropriate for evaluating the
underlying codec RD efficiency rather than the combined efficiency of codec plus
post-processing.

## When to use NEG

Use NEG when:

- **Comparing two codecs** where one or both apply in-loop sharpening that could
  inflate its standard VMAF score. NEG gives a fairer comparison of the underlying
  compression quality.
- **Comparing encoder configurations** (presets, rate-control strategies) within one
  codec where some configurations enable adaptive sharpening. NEG makes the curves
  comparable.
- **Evaluating codecs against a visually-lossless anchor** (e.g. target VMAF 95+).
  Sharpening-boosted scores can conceal coding artefacts that are visible in critical
  viewing; NEG surfaces them.

Example: `vmaf-tune compare --src source.yuv --neg --target-vmafs 94,96,97,98`

## When NOT to use NEG

Do not use NEG for:

- **Production quality monitoring against absolute baselines.** If your delivery
  specification says "VMAF >= 93 for all segments", the threshold was almost certainly
  calibrated against standard VMAF scores. Using NEG will produce lower scores on the
  same content and lead to false alarms or unnecessarily high bitrates.
- **Archival quality gating where the original reference is enhancement-free.** NEG
  penalises all sharpening relative to the reference, including benign codec sharpness
  recovery that genuinely improves perceived quality versus a blurry encode.
- **Live monitoring dashboards.** The NEG score is not directly comparable to
  operator-facing SLA thresholds that were set using standard VMAF.

## Model files

| Model string | File | Use case |
|---|---|---|
| `vmaf_v0.6.1neg` | `model/vmaf_v0.6.1neg.json` | HD (up to 1080p) content |
| `vmaf_4k_v0.6.1neg` | `model/vmaf_4k_v0.6.1neg.json` | UHD / 4K content |

Both files are in-tree in the `model/` directory. Float-precision variants
(`vmaf_float_v0.6.1neg.pkl`, `vmaf_float_v0.6.1neg.pkl.model`) are also present for
the Python training harness.

## Using NEG in vmaf-tune

The `--neg` flag is available on the following `vmaf-tune` subcommands:

| Subcommand | Effect |
|---|---|
| `recommend` | Routes the scorer to the NEG model |
| `tune-per-shot` | Each per-shot bisect uses the NEG model |
| `compare` | All codec bisects in the run use the NEG model |
| `ladder` | The convex-hull sampler scores with the NEG model |
| `corpus` | The Phase A grid sweep scores with the NEG model |

When `--neg` is passed together with `--vmaf-model vmaf_v0.6.1`, the model is routed
to `vmaf_v0.6.1neg`. When `--vmaf-model vmaf_4k_v0.6.1` is set (or selected
automatically for 4K content), the model routes to `vmaf_4k_v0.6.1neg`.

### Example: codec comparison with NEG

```bash
vmaf-tune compare \
    --src reference.yuv \
    --width 1920 --height 1080 \
    --neg \
    --target-vmafs 94,96,97,98 \
    --encoders libx264,libx265,libsvtav1 \
    --format json \
    --output compare_neg.json
```

### Example: per-shot tuning with NEG

```bash
vmaf-tune tune-per-shot \
    --src reference.mp4 \
    --encoder libx265 \
    --target-vmaf 93 \
    --neg
```

### Example: bitrate ladder with NEG

```bash
vmaf-tune ladder \
    --src reference.yuv \
    --resolutions 1920x1080,1280x720,854x480 \
    --target-vmafs 95,90,85 \
    --encoder libsvtav1 \
    --neg \
    --format hls
```

## Using NEG directly with the vmaf CLI

The libvmaf CLI accepts a `--model version=vmaf_v0.6.1neg` flag:

```bash
vmaf \
    --reference reference.yuv \
    --distorted distorted.yuv \
    --width 1920 --height 1080 \
    --pixel_format 420 --bitdepth 8 \
    --model version=vmaf_v0.6.1neg \
    --json --output scores.json
```

## Score interpretation

NEG scores are calibrated on a 0–100 scale, the same as standard VMAF. However,
NEG scores are **systematically lower than standard VMAF** on the same content when
the encoder uses any form of sharpening. This is by design — NEG is measuring a
different quantity (codec compression efficiency without enhancement gain).

Do not compare NEG scores directly against standard VMAF thresholds or published
quality grades. Establish NEG-specific operating points from your encoder sweep data.

## Technical references

- ADR-0616: [VMAF NEG integration — design](adr/../adr/0616-vmaf-neg-integration.md)
- ADR-0622: [VMAF NEG integration — implementation](adr/../adr/0622-vmaf-neg-integration-impl.md)
- Netflix Tech Blog: "Toward a Better Quality Metric for Streaming" (describes NEG
  motivation; search Netflix Tech Blog for the article).
- `model/vmaf_v0.6.1neg.json` — in-tree SVM model (HD).
- `model/vmaf_4k_v0.6.1neg.json` — in-tree SVM model (4K).

<!-- markdownlint-disable MD060 -->
# vmaf-roi-score — saliency-weighted VMAF scoring

`vmaf-roi-score` is a fork-local Python tool that produces a
**region-of-interest VMAF score** by combining a standard full-frame
VMAF run with a saliency-masked variant. Useful when a downstream
caller wants to weight quality measurements toward salient regions
(faces, focal subjects) rather than treat every pixel as equally
important.

> **Don't confuse this with `vmaf-roi`.** That binary
> (`core/tools/vmaf_roi.c`, ADR-0247) consumes a saliency map and
> emits **encoder QP-offset sidecars** for x265 / SVT-AV1. `vmaf-roi-score`
> is the *scoring*-side counterpart — same model lineage, different
> output.
>
> | Tool | Output | Surface |
> |---|---|---|
> | `vmaf-roi` (ADR-0247) | per-CTU QP offsets (encoder steering) | C binary |
> | `vmaf-roi-score` (ADR-0296) | saliency-weighted VMAF score | Python tool |

## What it computes

```text
roi_vmaf = (1 - w) * vmaf_full + w * vmaf_masked
```

- `vmaf_full` — pooled VMAF on the full frame (standard libvmaf run).
- `vmaf_masked` — pooled VMAF on a saliency-masked variant of the
  distorted YUV: low-saliency pixels are replaced with reference-
  matching pixels, so the masked run's score is dominated by the
  salient region.
- `w ∈ [0, 1]` — caller-controlled weight. `w=0` returns the standard
  full-frame VMAF unchanged; `w=1` returns the masked VMAF only.

## What it explicitly does **not** do

- **It is not "true" per-pixel saliency-weighted VMAF.** The masked
  run is a pixel substitution, not a per-feature weight. A salient
  region surrounded by a "perfect-match" zone scores differently than
  the same region under a *zero-weight* zone — VMAF's edge-sensitive
  features (motion, ADM) react to the boundary between the salient
  region and the substituted background. Option A (per-pixel feature
  pooling weighted by saliency in libvmaf C code) is tracked as
  deferred work in ADR-0296.
- **It makes no claim about MOS correlation.** This tool is a
  prototype that exposes a saliency-weighted *signal*; whether that
  signal tracks subjective quality better than uniform VMAF is an
  open research question. Validation against a labelled MOS dataset
  is a separate exercise — see Research-0063 §"What we deliberately
  don't measure".

## Install

```bash
pip install -e 'tools/vmaf-roi-score[runtime]'
```

The `vmaf` binary must be on `$PATH` (or pass `--vmaf-bin` explicitly).

## Quick start (saliency model)

Use `saliency_student_v1` for the saliency mask and keep the default
threshold/fade unless you are calibrating a corpus-specific policy.

```bash
vmaf-roi-score \
    --reference path/to/ref.yuv --distorted path/to/dis.yuv \
    --width 1920 --height 1080 --pix-fmt yuv420p \
    --saliency-model model/tiny/saliency_student_v1.onnx \
    --threshold 0.3 --fade 0.1 --weight 0.5
```

The tool writes a temporary distorted YUV where low-saliency pixels are
replaced by reference pixels, runs `vmaf` on that masked file, and
blends the full-frame and masked scores.

## Synthetic-mask smoke

The synthetic-mask path skips ONNX inference but still materialises a
masked distorted YUV through the same plane-blending code path. Use it
to verify your install, mask materialisation, VMAF subprocess calls,
and combine math without depending on an ONNX model.

```bash
vmaf-roi-score \
    --reference path/to/ref.yuv --distorted path/to/dis.yuv \
    --width 1920 --height 1080 --pix-fmt yuv420p \
    --synthetic-mask 0.5 --weight 0.5
```

Output (JSON to stdout):

```json
{
  "schema_version": 1,
  "vmaf_full": 80.0,
  "vmaf_masked": 90.0,
  "weight": 0.5,
  "vmaf_roi": 85.0,
  "model": "vmaf_v0.6.1",
  "saliency_model": "synthetic",
  "reference": "path/to/ref.yuv",
  "distorted": "path/to/dis.yuv"
}
```

In synthetic mode `--synthetic-mask FILL` supplies a constant mask.
`--threshold` and `--fade` are applied the same way as they are for a
real saliency model, so `--threshold 0 --fade 1 --synthetic-mask 0.5`
blends each plane halfway between the reference and distorted inputs.

## Mask Materialisation

`--saliency-model` supports little-endian planar YUV formats:
`yuv420p`, `yuv422p`, `yuv444p`, and the matching 10/12/16-bit
variants such as `yuv420p10le`, `yuv422p12le`, and `yuv444p16le`.
High-bit-depth planes are blended at their native sample depth; the
reference frame is converted down to 8-bit RGB only for saliency-model
inference. Big-endian high-bit-depth YUV is not supported.

The mask is inferred from the reference frame, not the distorted frame.
That keeps saliency tied to scene content rather than compression
artefacts. `--threshold` controls where the tool starts preserving the
distorted pixels; `--fade` controls the soft transition above that
threshold. With the defaults, mask values below `0.3` are replaced by
reference pixels, values at `0.4` and above keep distorted pixels, and
the interval between them is blended.

## Flags

| Flag | Default | Meaning |
|---|---|---|
| `--reference PATH` | (required) | raw reference YUV |
| `--distorted PATH` | (required) | raw distorted YUV |
| `--width N`, `--height N` | (required) | YUV dimensions |
| `--pix-fmt FMT` | `yuv420p` | ffmpeg pix_fmt; mapped to libvmaf's `--pixel_format` |
| `--saliency-model PATH` | none | path to saliency ONNX (mutually exclusive with `--synthetic-mask`) |
| `--synthetic-mask FILL` | none | constant-value mask in `[0, 1]`; skips ONNX inference but still writes and scores a masked YUV (testing only; mutually exclusive with `--saliency-model`) |
| `--weight W` | `0.5` | saliency-masked component weight in `[0, 1]` |
| `--threshold T` | `0.3` | saliency cutoff used by `--saliency-model` |
| `--fade F` | `0.1` | soft fade band above `--threshold`; `0` makes a hard mask |
| `--model NAME` | `vmaf_v0.6.1` | VMAF model version passed through to `vmaf` |
| `--vmaf-bin PATH` | `vmaf` | location of the libvmaf CLI binary |
| `--output PATH` | stdout | write JSON result to this path |

## Exit codes

| Code | Meaning |
|---|---|
| 0 | success |
| 64 | saliency-mask materialisation failed (missing runtime deps, unsupported pix_fmt, bad mask shape) |
| 65 | `vmaf` ran but produced JSON missing the pooled scalar |
| other | passed through from the underlying `vmaf` invocation |

## See also

- [ADR-0296](../adr/0296-vmaf-roi-saliency-weighted.md) — the design ADR
  for this tool.
- [ADR-0425](../adr/0425-vmaf-roi-score-saliency-materialiser.md) — the
  saliency-mask materialiser follow-up.
- [Research-0069](../research/0069-vmaf-roi-saliency-weighted.md) —
  Option A vs B vs C decision matrix.
- [`docs/usage/vmaf-roi.md`](vmaf-roi.md) — the encoder-steering
  sibling tool (different surface, related model).
- [`docs/ai/models/saliency_student_v1.md`](../ai/models/saliency_student_v1.md)
  — the saliency model this tool consumes by default.

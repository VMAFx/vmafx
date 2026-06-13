# MobileSal saliency (no-reference scoring-side extractor)

`vmaf_tiny_mobilesal_placeholder_v0` — the historical smoke checkpoint
for the no-reference saliency feature extractor. The extractor runs a
tiny ONNX saliency model over the distorted frame and emits the **mean
of its per-pixel saliency map** as a scalar feature named
`saliency_mean`. It is the scoring-side surface for
[Wave 1 §2.3 of the tiny-AI roadmap](../roadmap.md), the first half of
backlog item T6-2 (T6-2a). Encoder-side ROI tooling (`tools/vmaf-roi`,
per-CTU QP-offset sidecars) is shipped as T6-2b.

> **Status — legacy smoke checkpoint.** The placeholder matches the
> MobileSal I/O contract, but emits ~constant saliency. Production
> saliency now uses the fork-trained
> [`saliency_student_v1`](saliency_student_v1.md) weights, which keep the
> same `input` / `saliency_map` tensor names and run through the same
> `feature_mobilesal.c` extractor. The original upstream MobileSal swap
> remains deferred by
> [ADR-0257](../../adr/0257-mobilesal-real-weights-deferred.md) because
> upstream weights are CC BY-NC-SA 4.0, Google-Drive-walled, and RGB-D.
> U-2-Net `u2netp` was also surveyed in
> [ADR-0265](../../adr/0265-u2netp-saliency-replacement-blocked.md);
> the fork-trained student is the license-clean production path.

Upstream paper: Wu, Liu, Cheng, Lu, Cheng, *"MobileSal: Extremely
Efficient RGB-D Salient Object Detection"*, IEEE TPAMI 2021.

## What the output means

The extractor emits a single feature named `saliency_mean`, one value
per frame (the distorted frame is the input — saliency is no-reference).

| Value | Interpretation |
| --- | --- |
| **~0.0** | Flat / featureless content; no salient subject |
| **~0.2 – 0.4** | Typical natural-content frame |
| **~0.5** | Foreground subject occupies a sizeable fraction of the frame |
| **~0.8+** | Subject dominates; mostly-salient content |
| **1.0** | Saturated (every pixel maxed) — usually a sign of model misuse |

Saliency-mean is **not** a quality score on its own — it is a content
descriptor. Downstream consumers correlate `saliency_mean` against
existing metric features (e.g. `vmaf`, `lpips`, `psnr`) to study how
foreground-vs-background distortion affects subjective quality.

The full saliency map is computed internally to derive the mean; it is
intentionally not exposed as a per-pixel feature in T6-2a. The encoder
side (`tools/vmaf-roi` per-CTU QP-offset sidecar) consumes the same
model in T6-2b and exports the map in encoder-native format.

## Shipped checkpoint

| Field | Value |
| --- | --- |
| Model id | `mobilesal_placeholder_v0` |
| Display name | `vmaf_tiny_mobilesal_placeholder_v0` |
| Location | `model/tiny/mobilesal.onnx` |
| Size | 330 bytes (synthetic placeholder) |
| SHA-256 | `f122631089977c4be7d60b9bf3d4daf186d275bd0587db2c9878578e006b91d4` |
| ONNX opset | 17 |
| Upstream source (paper) | [yuhuan-wu/MobileSal](https://github.com/yuhuan-wu/MobileSal) (HEAD `8f42ded5`; not currently shippable — see ADR-0257) |
| License (placeholder) | BSD-3-Clause-Plus-Patent (this fork) |
| License (upstream MobileSal weights) | CC BY-NC-SA 4.0 — **incompatible with the fork**; per `yuhuan-wu/MobileSal/README.md` §License. ADR-0218's MIT claim was inaccurate; corrected here and in ADR-0257. |
| Exporter (placeholder) | `scripts/gen_mobilesal_placeholder_onnx.py` |
| Registry entry | `mobilesal_placeholder_v0` in `model/tiny/registry.json` (smoke=true) |
| Status | Legacy smoke placeholder — superseded for production by `saliency_student_v1` |

The placeholder ONNX is deterministic (no `doc_string`, fixed
`producer_version`, deterministic protobuf serialisation) so the
sha256 stays stable across re-runs of the export script.

For content-dependent saliency, point the extractor at
`model/tiny/saliency_student_v1.onnx` (or the staged v2 ablation after
its ROI validation lands). The placeholder is retained to keep the
historical ABI / I/O-contract smoke path available.

## Input / output contract

The C extractor binds tensors by name, so any future drop-in (real
upstream MobileSal export, distilled student, etc.) must declare the
exact same names:

```text
inputs:
  input         float32[1, 3, H, W]   ImageNet-normalised RGB, NCHW
outputs:
  saliency_map  float32[1, 1, H, W]   per-pixel saliency in [0, 1]
```

`H` and `W` are dynamic — both the placeholder and the upstream graph
match whatever resolution the C side feeds. ImageNet normalisation
(mean `[0.485, 0.456, 0.406]`, std `[0.229, 0.224, 0.225]`) is applied
in the C side via the shared `vmaf_tensor_from_rgb_imagenet()` helper,
identical to LPIPS's wiring (see [`lpips_sq.md`](lpips_sq.md)).

## Usage — CLI

```bash
vmaf \
    --reference ref.yuv \
    --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --feature mobilesal \
    --feature_params mobilesal:model_path=model/tiny/saliency_student_v1.onnx \
    --output score.json
```

The output JSON gains a per-frame `saliency_mean` column alongside any
other features requested in the same run. Combine with `lpips` and
`vmaf` for the full saliency-quality picture:

```bash
vmaf --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --feature vmaf \
    --feature lpips \
    --feature_params lpips:model_path=model/tiny/lpips_sq.onnx \
    --feature mobilesal \
    --feature_params mobilesal:model_path=model/tiny/saliency_student_v1.onnx \
    --output combined.json
```

Equivalently, set the model path via env var:

```bash
VMAF_MOBILESAL_MODEL_PATH=model/tiny/saliency_student_v1.onnx \
    vmaf --reference ref.yuv --distorted dist.yuv \
        --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
        --feature mobilesal --output score.json
```

## Usage — C API

```c
#include <libvmaf/libvmaf.h>

VmafFeatureDictionary *opts = NULL;
vmaf_feature_dictionary_set(&opts, "model_path", "model/tiny/saliency_student_v1.onnx");
int err = vmaf_use_feature(ctx, "mobilesal", opts);
/* ... vmaf_score_pooled(ctx, ..., "saliency_mean", ...) for the per-frame mean */
```

Equivalent to setting `VMAF_MOBILESAL_MODEL_PATH` before
`vmaf_use_feature(ctx, "mobilesal", NULL)`.

## Known limitations

- **Bit depth — 8-bit YUV only**: `mobilesal` rejects non-8-bit input at
  `init()` time with `-ENOTSUP` and an actionable error message naming this
  extractor as the blocker. The saliency ONNX model requires 8-bit
  ImageNet-normalised RGB; 10-bit and 12-bit support would require retraining
  and is not planned (see ADR-0613 §P1-3 rationale). If you pass `--bitdepth
  10` (or `--bitdepth 12`) together with `--feature mobilesal`, the run will
  abort before scoring with a message like:

  ```text
  mobilesal: bpc=10 is not supported (8-bit only). The mobilesal extractor
  requires 8-bit YUV input because the saliency model was trained on 8-bit
  ImageNet-RGB. Use --bitdepth 8 or omit --feature mobilesal for HDR /
  10-bit / 12-bit content.
  ```

  **Workaround**: drop `--feature mobilesal` from HDR / 10-bit / 12-bit
  scoring runs, or score with `--bitdepth 8` if the source is actually 8-bit
  content muxed into a 10-bit container.
- **Pixel format**: `YUV420P`, `YUV422P`, `YUV444P` accepted;
  `YUV400P` (luma-only) is rejected at `init()` because the model
  requires three RGB channels.
- **Colour space**: BT.709 limited-range Y'CbCr → RGB at the C side,
  matching `feature_lpips.c`. BT.2020 / full-range is approximate
  (deliberate trade-off — see `feature_mobilesal.c` comment).
- **Resolution**: bounded by the selected ONNX graph's dynamic shape.
  The placeholder has no useful quality floor; the fork-trained student
  was trained on 256×256 crops.
- **CPU vs GPU path**: served via `vmaf_dnn_session_run()` which picks
  CPU EP by default; CUDA EP is used automatically when libvmaf is
  built with `-Denable_cuda=true` and the graph is supported.
- **Score interpretation**: with the placeholder, `saliency_mean` is
  ~0.5 regardless of input — the placeholder exists to lock down the
  pipeline, not to score quality. With `saliency_student_v1`, the score
  is content-dependent.

## How the placeholder is regenerated

```bash
python scripts/gen_mobilesal_placeholder_onnx.py
# wrote model/tiny/mobilesal.onnx (330 bytes, sha256=f1226...)
# wrote model/tiny/mobilesal.json
# updated model/tiny/registry.json
```

Re-running on the same `numpy` / `onnx` versions produces byte-identical
output. CI verifies the sha256 against `registry.json` before
`CreateSession`.

## Related

- [`lpips_sq.md`](lpips_sq.md) — sister full-reference DNN extractor;
  shares the YUV → ImageNet-RGB plumbing.
- [`../roadmap.md`](../roadmap.md) §2.3 — Wave 1 MobileSal scope.
- [`saliency_student_v1.md`](saliency_student_v1.md) — production
  fork-trained saliency weights for this extractor.
- [`saliency_student_v2.md`](saliency_student_v2.md) — staged higher-IoU
  resize-decoder ablation, pending ROI A/B validation before a
  production flip.
- [ADR-0218](../../adr/0218-mobilesal-saliency-extractor.md) — design
  notes (smoke-only placeholder, scoring-vs-encoder split, scalar-vs-map
  output).
- [ADR-0257](../../adr/0257-mobilesal-real-weights-deferred.md) —
  blocker decision deferring the T6-2a-followup real-weights swap.
- [Research-0053](../../research/0053-mobilesal-real-weights-blocker.md)
  — upstream survey, licence analysis, and alternatives walk.
  first blocker: upstream MobileSal license + distribution +
  RGB-D mismatch.
- [ADR-0265](../../adr/0265-u2netp-saliency-replacement-blocked.md)
  — second blocker: U-2-Net `u2netp` distribution + op-allowlist
  mismatch.
- [Research-0054](../../research/0055-u2netp-saliency-replacement-survey.md)
  — companion survey for ADR-0265.
- [ADR-0286](../../adr/0286-saliency-student-fork-trained-on-duts.md)
  — fork-trained production saliency-student path.
- [ADR-0042](../../adr/0042-tinyai-docs-required-per-pr.md) — tiny-AI
  doc-substance rule this page satisfies.

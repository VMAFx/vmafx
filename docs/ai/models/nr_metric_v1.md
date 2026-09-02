# `nr_metric_v1` — tiny no-reference quality metric

`nr_metric_v1` is a compact MobileNet-style no-reference (NR) quality
estimator that predicts a MOS-proxy scalar from a single 224×224 grayscale
luma frame. It is the C2 baseline for the fork's tiny-AI NR capability
([ADR-0020](../../adr/0020-tinyai-four-capabilities.md)), trained on
KoNViD-1k crowd-sourced MOS labels.

> **Status — shipped 2026-04-25.** Production baseline for C2 NR scoring
> (KoNViD-1k, CC BY 4.0). An INT8 sidecar is available via
> `nr_metric_v1.int8.onnx` (dynamic-PTQ). See
> [ADR-0168](../../adr/0168-tinyai-konvid-baselines.md) and
> [ADR-0174](../../adr/0174-first-model-quantisation.md).

## What the output means

A single scalar per frame on a normalised MOS scale. The model was
trained to predict crowd-sourced MOS (1–5 scale) from KoNViD-1k; the
output is a continuous float in approximately that range.

| Value | Interpretation |
| --- | --- |
| **~4.5–5.0** | Pristine / near-reference quality |
| **~3.5–4.5** | Good quality; minor perceptible artefacts |
| **~2.5–3.5** | Moderate quality; clearly visible artefacts |
| **~1.0–2.5** | Poor quality; heavy compression / blur |

The output is a frame-level MOS estimate. Clip-level quality is
typically obtained by averaging over all frames (or a representative
subset). The model is **content-blind** — it does not have access to the
reference stream.

## Shipped checkpoint

| Field | Value |
| --- | --- |
| Model id | `nr_metric_v1` |
| Location | `model/tiny/nr_metric_v1.onnx` |
| INT8 sidecar | `model/tiny/nr_metric_v1.int8.onnx` |
| Architecture | MobileNet-tiny — depthwise separable Conv stack; ~19 K params |
| Input | `input` — float32 NCHW `[1, 1, 224, 224]` grayscale luma in `[0, 1]` |
| Output | `mos_score` — float32 `[1]` scalar MOS estimate |
| ONNX opset | 17 |
| Training corpus | KoNViD-1k (1 200 clips; CC BY 4.0; not redistributed in-tree) |
| Val MSE | ~0.382 (RMSE ≈ 0.62 on 1–5 MOS, KoNViD-1k validation split) |
| Quantisation | Dynamic-PTQ INT8 via `ai/scripts/ptq_dynamic.py`; `quant_accuracy_budget_plcc = 0.01` |
| License | BSD-3-Clause-Plus-Patent |
| Trainer / exporter | `ai/scripts/train_konvid.py` + `ai/scripts/export_tiny_models.py` |

Fresh exports from `ai/scripts/export_tiny_models.py` add ADR-0661
`run_provenance` to `model/tiny/nr_metric_v1.json`. That block records the
checkpoint input, parsed exporter arguments, ONNX output, sidecar output, and
registry target so a refreshed C2 baseline can be traced without relying on
shell history.

## Training corpus provenance

| Field | Value |
| --- | --- |
| Dataset | KoNViD-1k |
| Source | <https://datasets.vqa.mmsp-kn.de/databases/KoNViD-1k/> |
| Licence | CC BY 4.0 — clips are not redistributed in-tree |
| Clips | 1 200 user-generated video clips, 8 s each at various resolutions |
| MOS labels | Crowd-sourced mean opinion score (1–5 scale, Amazon Mechanical Turk) |
| Split used | ~973 train / ~107 val / ~120 test (80/9/10 % random split, seed 42) |
| Feature | Middle frame extracted per clip at 224×224 grayscale |

**Acknowledgement.** This model was trained on KoNViD-1k. We thank the
dataset authors for distributing the clips and MOS labels under CC BY 4.0.
The clips themselves are not committed to this repository.

## Op-allowlist conformance

Every op in the graph is on
[`core/src/dnn/op_allowlist.c`](../../../core/src/dnn/op_allowlist.c):
`Conv`, `Relu`, `DepthwiseConv`, `GlobalAveragePool`, `Flatten`,
`Gemm` (or equivalent `MatMul` + `Add`).

## Usage — CLI

```bash
vmaf \
    --reference ref.yuv \
    --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --tiny-model model/tiny/nr_metric_v1.onnx \
    --output score.json
```

The output JSON gains a per-frame `nr_mos_score` column. Pool to clip
level by averaging across frames.

## Usage — C API

```c
#include <libvmaf/libvmaf.h>

VmafDnnSession *session = NULL;
vmaf_dnn_session_open(&session, "model/tiny/nr_metric_v1.onnx",
                      VMAF_DNN_DEVICE_CPU);
/* run session per-frame ... */
vmaf_dnn_session_close(session);
```

## Reproducing the model

```bash
# 1. fetch KoNViD-1k (~40 GB) — not redistributed in-tree
.venv/bin/python ai/scripts/fetch_konvid_1k.py

# 2. extract middle frames + convert to corpus JSONL
.venv/bin/python ai/scripts/konvid_1k_to_corpus_jsonl.py

# 3. train the C2/C3 checkpoints
.venv/bin/python ai/scripts/train_konvid.py \
    --model both \
    --output-c2 runs/c2_konvid \
    --output-c3 runs/c3_konvid \
    --epochs-c2 50 \
    --epochs-c3 200 \
    --seed 42

# 4. export the ONNX + sidecar + registry rows
.venv/bin/python ai/scripts/export_tiny_models.py \
    --c2-ckpt runs/c2_konvid/last.ckpt \
    --c3-ckpt runs/c3_konvid/last.ckpt

# 5. quantise to INT8
.venv/bin/python ai/scripts/ptq_dynamic.py \
    --model model/tiny/nr_metric_v1.onnx \
    --output model/tiny/nr_metric_v1.int8.onnx

# 6. validate against the registry
.venv/bin/python ai/scripts/validate_model_registry.py
```

## Fast-NR calibration sidecar

`vmaf-tune --fast-nr` reads `model/tiny/nr_metric_v1.json` for the optional
calibration fields used to skip full-reference calls during CRF bisection:
`calibration_slope`, `calibration_intercept`, and `calibration_threshold`.
The slope/intercept map this model's raw MOS-like output into VMAF units before
the threshold comparison. Regenerate those fields with:

```bash
.venv/bin/python ai/scripts/calibrate_nr_threshold.py \
    --corpus .corpus/netflix/ \
    --output model/tiny/nr_metric_v1.json \
    --nr-ep cpu
```

Use `--nr-ep cpu` when a long CUDA/ROCm extraction job is already running or
when calibration should be bit-for-bit reproducible across hosts. The default
`--nr-ep auto` tries CUDA/ROCm ONNX Runtime providers first and falls back to
CPU. When the corpus path contains a `ref/` directory, calibration sweeps only
that reference directory; the local Netflix public source names are recognised
as 1920×1080 YUV even though the filenames omit the geometry.

Fresh sidecars include ADR-0661 `run_provenance` with the requested and actual
corpus directories, `nr_metric_v1.onnx`, CRF grid, parsed CLI arguments, JSON
output path, and Markdown calibration report path.

## Known limitations

- **Single-frame, no temporal context**: quality of slow-motion blur,
  flicker, or buffering artefacts may be underestimated relative to
  human perception, which integrates over ≥1 s of video.
- **Grayscale only**: chroma degradation (colour bleeding, banding in
  blue channel) contributes nothing to the prediction.
- **KoNViD-1k domain**: the corpus is user-generated internet content at
  moderate bitrates. Performance on professionally shot content, HDR/WCG
  material, or severe synthetic degradation outside the training
  distribution may be unreliable.
- **MOS scale calibration**: the 1–5 scale is calibrated to KoNViD-1k's
  specific test conditions. Direct comparison to VMAF scores or other
  dataset MOS values requires dataset-specific recalibration.

## Related

- [`learned_filter_v1.md`](learned_filter_v1.md) — sibling KoNViD-1k
  baseline (C3 residual filter, same training corpus).
- [ADR-0168](../../adr/0168-tinyai-konvid-baselines.md) — decision record
  for both C2 + C3 KoNViD baselines.
- [ADR-0174](../../adr/0174-first-model-quantisation.md) — INT8
  dynamic-PTQ policy.
- [ADR-0248](../../adr/0248-nr-metric-v1-ptq.md) — PTQ accuracy budget
  for this model.
- [ADR-0042](../../adr/0042-tinyai-docs-required-per-pr.md) — tiny-AI
  doc-substance rule this card satisfies.

# `nr_metric_v1` — tiny no-reference quality metric

`nr_metric_v1` is a compact MobileNet-style no-reference (NR) quality
estimator that predicts a MOS-proxy scalar from a single 224×224 grayscale
luma frame. It is the C2 baseline for the fork's tiny-AI NR capability
([ADR-0020](../../adr/0020-tinyai-four-capabilities.md)), trained on
KoNViD-1k crowd-sourced MOS labels.

> **Status — shipped 2026-04-25.** Production baseline for C2 NR scoring
> (KoNViD-1k). An INT8 sidecar is available via
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
| SHA-256 (fp32) | `75eff676bff0f0f911fd59ac72c90240e95caff67302b37f6e35bfbeac2b680b` |
| SHA-256 (INT8) | `e5ba2086f53c74539902883b12b01d83f06806f084b692b1d5ad325f9300cab2` |
| Location | `model/tiny/nr_metric_v1.onnx` |
| INT8 sidecar | `model/tiny/nr_metric_v1.int8.onnx` |
| Architecture | MobileNet-tiny — depthwise separable Conv stack; ~19 K params |
| Input | `frame` — float32 NCHW `[batch, 1, 224, 224]` grayscale luma in `[0, 1]` |
| Output | `mos` — float32 `[batch]` scalar MOS estimate |
| ONNX opset | 18 (the file, the registry and the sidecar agree; torch's exporter raises a requested 17 to 18) |
| Training corpus | KoNViD-1k (1 200 clips; not redistributed in-tree; terms below) |
| Val MSE | ~0.382 (RMSE ≈ 0.62 on 1–5 MOS, KoNViD-1k validation split) |
| Quantisation | Dynamic-PTQ INT8 via `ai/scripts/ptq_dynamic.py`; `quant_accuracy_budget_plcc = 0.01` |
| License | BSD-2-Clause-Patent |
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
| Source | <http://database.mmsp-kn.de/konvid-1k-database.html> |
| Terms | No licence named: the database page says KoNViD-1k "is freely available to the research community" (read 2026-10-04); its clips are YFCC100M videos under assorted Creative Commons licences. Clips are not redistributed in-tree; the terms and the fork's reading are under [Training data terms](#training-data-terms). |
| Clips | 1 200 user-generated video clips, 8 s each at various resolutions |
| MOS labels | Crowd-sourced mean opinion score (1–5 scale, Amazon Mechanical Turk) |
| Split used | ~973 train / ~107 val / ~120 test (about 80/9/10 %; the trainer's default is 80/10/10 with seed 42) |
| Feature | Middle frame extracted per clip at 224×224 grayscale |

**Acknowledgement.** This model was trained on KoNViD-1k. We thank the
dataset authors for distributing the clips and MOS labels.
The clips themselves are not committed to this repository.

## Training data terms

This model was trained on KoNViD-1k. The terms below are quoted as each source
states them (read 2026-10-04); the [dataset
terms](../training-data.md#dataset-terms) list where each comes from and which
models it trained.

**KoNViD-1k**:
<http://database.mmsp-kn.de/konvid-1k-database.html>

> KoNViD-1k is freely available to the research community.
>
> We took YFCC100m as a baseline database, consisting of 793436 Creative Commons
> (CC) video sequences

The page names no licence and offers the database to the research community;
each clip keeps the Creative Commons licence of its YFCC100M upload.

**Reading.** The fork ships these weights under BSD-2-Clause-Patent: they are
fitted parameters that cannot reproduce a clip, an image or a label, and no
dataset file is redistributed. That is the fork's reading, not a permission
from the dataset's authors. Where a dataset limits its use to research and
that limit binds the weights where you use them, treat the model as
research-only.

**Retrain.** RC9 retrains this model on data cleared for redistribution
(`T-TINY-AI-RETRAIN-CLEARED-DATA-2026-10-04` in [state](../../state.md);
[ADR-1490](../../adr/1490-rc3-rc9-candidate-map-cpu-capability.md),
[ADR-1570](../../adr/1570-tiny-model-dataset-terms-retrain-rc9.md)).

## Op-allowlist conformance

Every op in the graph is on
[`core/src/dnn/op_allowlist.c`](../../../core/src/dnn/op_allowlist.c). The
shipped fp32 graph uses `Conv` (depthwise layers are `Conv` with a group
count), `Clip`, `Concat`, `Gemm`, `ReduceMean`, `Reshape`, `Shape` and
`Squeeze`. There is no separate `DepthwiseConv` operator on the allowlist.

## Usage — CLI

```bash
vmaf \
    --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --no-reference \
    --tiny-model model/tiny/nr_metric_v1.onnx \
    --tiny-resize bilinear \
    --output score.json
```

Three options matter for this model:

- `--no-reference` runs in no-reference mode (the model sees only the
  distorted stream, so `--reference` can be omitted).
- `--tiny-resize bilinear|nearest|bicubic` is required whenever the clip is not
  224×224: the model input is fixed at 224×224, and without the option a
  size mismatch is a hard error (`-ERANGE`, "problem scoring picture 0").
  The three filters give scores that differ by about 2 %, so record the filter
  next to any reported number.
- `--tiny-model` takes a path to the ONNX file.

The score is attached under the sidecar's `name` field when it has one,
otherwise under the feature name `vmaf_tiny_model`. `nr_metric_v1.json` has no
`name`, so the per-frame values appear as `vmaf_tiny_model` in the JSON
`frames` array and in `pooled_metrics`. Pool to clip level by averaging across
frames. On the 576×324 Netflix pair (`src01_hrc01`, 48 frames) the pooled mean
of `vmaf_tiny_model` is about 3.37.

## Usage — C API

```c
#include <libvmaf/libvmaf.h>

VmafDnnSession *session = NULL;
VmafDnnConfig cfg = {.device = VMAF_DNN_DEVICE_CPU};  /* NULL = auto */
vmaf_dnn_session_open(&session, "model/tiny/nr_metric_v1.onnx", &cfg);
/* run session per-frame ... */
vmaf_dnn_session_close(session);
```

## Reproducing the model

1. Fetch KoNViD-1k (~40 GB; not redistributed in-tree):

   ```bash
   .venv/bin/python ai/scripts/fetch_konvid_1k.py
   ```

2. Extract middle frames and convert to corpus JSONL:

   ```bash
   .venv/bin/python ai/scripts/konvid_1k_to_corpus_jsonl.py
   ```

3. Train the C2/C3 checkpoints:

   ```bash
   .venv/bin/python ai/scripts/train_konvid.py \
       --model both \
       --output-c2 runs/c2_konvid \
       --output-c3 runs/c3_konvid \
       --epochs-c2 50 \
       --epochs-c3 200 \
       --seed 42
   ```

4. Export the ONNX, sidecar and registry rows:

   ```bash
   .venv/bin/python ai/scripts/export_tiny_models.py \
       --c2-ckpt runs/c2_konvid/last.ckpt \
       --c3-ckpt runs/c3_konvid/last.ckpt
   ```

5. Quantise to INT8 (the fp32 ONNX path is a positional argument):

   ```bash
   .venv/bin/python ai/scripts/ptq_dynamic.py model/tiny/nr_metric_v1.onnx \
       --output model/tiny/nr_metric_v1.int8.onnx
   ```

6. Validate against the registry:

   ```bash
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

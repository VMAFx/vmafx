# `learned_filter_v1` — tiny residual luma filter

`learned_filter_v1` is a self-supervised residual convolutional neural
network that maps a degraded luma frame to a clean reconstruction. It
serves as the C3 baseline for the fork's tiny-AI filter capability
([ADR-0020](../../adr/0020-tinyai-four-capabilities.md)), exercising the
full training + export + quantisation pipeline end-to-end.

> **Status — shipped 2026-04-25.** Production baseline for the C3 filter
> capability (KoNViD-1k self-supervised). An INT8 sidecar is available via
> `learned_filter_v1.int8.onnx` (dynamic-PTQ). See
> [ADR-0168](../../adr/0168-tinyai-konvid-baselines.md) and
> [ADR-0174](../../adr/0174-first-model-quantisation.md).

## What the output means

The model takes a degraded luma frame (blurred + JPEG-compressed) and
produces a residual-corrected clean luma estimate. The output is the
**reconstructed luma tensor** on the same scale as the input; it is not
a quality score. Downstream consumers subtract the output from the input
to obtain the learned residual correction, or pass it directly to a
downstream feature extractor.

## Shipped checkpoint

| Field | Value |
| --- | --- |
| Model id | `learned_filter_v1` |
| Location | `model/tiny/learned_filter_v1.onnx` |
| INT8 sidecar | `model/tiny/learned_filter_v1.int8.onnx` |
| Architecture | 4-block residual CNN — Conv(1→16, 3×3) → 3×ResBlock(16, 3×3) → Conv(16→1, 3×3); ~19 K params |
| Input | `input` — float32 NCHW `[1, 1, H, W]` normalised luma in `[0, 1]` |
| Output | `output` — float32 NCHW `[1, 1, H, W]` reconstructed luma |
| ONNX opset | 17 |
| Training corpus | KoNViD-1k middle-frames (1 200 clips; not redistributed in-tree) |
| Val loss (L1) | ~0.019 on normalised luma (KoNViD-1k validation split) |
| Quantisation | Dynamic-PTQ INT8 via `ai/scripts/ptq_dynamic.py`; `quant_accuracy_budget_plcc = 0.01` |
| License | BSD-3-Clause-Plus-Patent |
| Trainer / exporter | `ai/scripts/export_tiny_models.py` |

Fresh exports from `ai/scripts/export_tiny_models.py` add ADR-0661
`run_provenance` to `model/tiny/learned_filter_v1.json`. That block records
the C3 checkpoint input, parsed exporter arguments, ONNX output, sidecar
output, and registry target so a refreshed filter baseline can be replayed.

## Training corpus provenance

| Field | Value |
| --- | --- |
| Dataset | KoNViD-1k |
| Source | <https://datasets.vqa.mmsp-kn.de/databases/KoNViD-1k/> |
| Licence | CC BY 4.0 — clips are not redistributed in-tree |
| Usage | Middle frame extracted per clip; synthetic degradation applied (Gaussian blur σ=1.2 + JPEG quality=35); self-supervised (degraded→clean pairs, no external MOS labels used for the filter task) |

**Acknowledgement.** Training uses KoNViD-1k frames for self-supervised
degradation recovery. The clips are not committed to this repository.

## Op-allowlist conformance

Every op in the graph is on
[`core/src/dnn/op_allowlist.c`](../../../core/src/dnn/op_allowlist.c):
`Conv`, `Relu`, `Add` (residual skip connection).

## Degradation recipe

The synthetic training pairs are produced inside `export_tiny_models.py`:

1. Load middle frame of each KoNViD-1k clip as 224×224 grayscale
   (nearest-neighbour crop; no random augment at export time).
2. Apply Gaussian blur with σ=1.2.
3. JPEG-compress at quality 35 using `PIL.Image.save(..., quality=35)`.
4. Luma pair: (degraded, original) both normalised to `[0, 1]`.

## Usage — `vmaf_pre` FFmpeg filter

`learned_filter_v1` is the filter loaded by `ffmpeg-patches/0002`
(`vmaf_pre`) when `--tiny-model=learned_filter_v1` is passed:

```bash
ffmpeg \
  -i ref.yuv -i dist.yuv \
  -filter_complex '[1:v]vmaf_pre=model_path=model/tiny/learned_filter_v1.onnx[d];
                   [0:v][d]libvmaf' \
  -f null -
```

The filter applies the model to the distorted stream's luma before VMAF
scoring, enabling a learned pre-processing step upstream of the feature
extractors.

## Reproducing the model

```bash
# 1. fetch KoNViD-1k (~40 GB) — not redistributed in-tree
.venv/bin/python ai/scripts/fetch_konvid_1k.py

# 2. train the C2/C3 checkpoints
#    A run-provenance sidecar is written automatically to
#    runs/c2_konvid/train_konvid.manifest.json (ADR-0668).
#    Pass --manifest-out <path> to override the sidecar location.
.venv/bin/python ai/scripts/train_konvid.py \
    --model both \
    --output-c2 runs/c2_konvid \
    --output-c3 runs/c3_konvid \
    --epochs-c2 50 \
    --epochs-c3 200 \
    --seed 42

# 3. export the ONNX + sidecar + registry rows
.venv/bin/python ai/scripts/export_tiny_models.py \
    --c2-ckpt runs/c2_konvid/last.ckpt \
    --c3-ckpt runs/c3_konvid/last.ckpt

# 4. quantise to INT8
.venv/bin/python ai/scripts/ptq_dynamic.py \
    --model model/tiny/learned_filter_v1.onnx \
    --output model/tiny/learned_filter_v1.int8.onnx

# 5. validate against the registry
.venv/bin/python ai/scripts/validate_model_registry.py
```

## Known limitations

- **Task scope**: the degradation recipe (Gaussian blur + JPEG) covers
  classic codec artefacts but not block noise patterns typical of
  AVC/HEVC at very low bitrate or content-adaptive quantisation.
- **Luma only**: the filter operates on the Y channel. Chroma artefacts
  (colour bleed, cross-component leakage) are not corrected.
- **Fixed crop**: training used 224×224 random crops; inference is
  fully convolutional (no size constraint), but quality on very large or
  very small inputs may degrade.
- **Self-supervised only**: no perceptual loss (LPIPS, SSIM); the L1
  reconstruction target may slightly over-smooth texture.

## Related

- [`nr_metric_v1.md`](nr_metric_v1.md) — sibling KoNViD-1k baseline (NR
  quality metric, same training corpus).
- [ADR-0168](../../adr/0168-tinyai-konvid-baselines.md) — decision record
  for both C2 + C3 KoNViD baselines.
- [ADR-0174](../../adr/0174-first-model-quantisation.md) — INT8
  dynamic-PTQ policy.
- [ADR-0042](../../adr/0042-tinyai-docs-required-per-pr.md) — tiny-AI
  doc-substance rule this card satisfies.

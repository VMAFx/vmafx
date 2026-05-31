# vmaf_tiny_v2 — feature-fusion VMAF estimator

`vmaf_tiny_v2` is a tiny multi-layer perceptron that predicts a VMAF
score from six classic libvmaf features
(`adm2`, `vif_scale0..3`, `motion2` — the canonical-6 set used by
`vmaf_v0.6.1`). It replaces `vmaf_tiny_v1` as the default tiny VMAF
fusion model: same input contract, same output range, +0.005–0.018
PLCC across the validation chain (Netflix LOSO + KoNViD 5-fold).

> The model is **only the regressor**. Feature extraction is unchanged
> — `adm`, `vif`, and `motion` are computed by the existing libvmaf
> CPU/GPU paths. v2 just gives you a smaller, more accurate fusion
> head than the upstream SVM.

## What the output means

A single scalar per frame, on the same 0–100 VMAF scale as the
classic SVM regressor.

| Value | Interpretation |
| --- | --- |
| **100** | Perceptually identical to the reference |
| **80–95** | High-quality encode |
| **60–80** | Visible compression artifacts |
| **< 60** | Heavy degradation |

## Shipped checkpoint

| Field | Value |
| --- | --- |
| Model name | `vmaf_tiny_v2` |
| Location | `model/tiny/vmaf_tiny_v2.onnx` |
| Architecture | `mlp_small` — Linear(6, 16) → ReLU → Linear(16, 8) → ReLU → Linear(8, 1), ~257 params |
| Input | `features` — float32 `[N, 6]`, dynamic batch |
| Feature order | `adm2, vif_scale0, vif_scale1, vif_scale2, vif_scale3, motion2` |
| Output | `vmaf` — float32 `[N]` |
| ONNX opset | 17 |
| Quantisation | fp32 (size already <2 KB; 8-bit has no shipping payoff) |
| License | BSD-3-Clause-Plus-Patent |
| Registry entry | `vmaf_tiny_v2` in `model/tiny/registry.json` |
| Sidecar | `model/tiny/vmaf_tiny_v2.json` |
| Exporter | `ai/scripts/export_vmaf_tiny_v2.py` |
| Trainer | `ai/scripts/train_vmaf_tiny_v2.py` |

The graph bakes the StandardScaler `(mean, std)` from the training
set as Constant nodes that run *before* the MLP — the runtime feeds
raw feature values, the trust-root sha256 covers the calibration
values too. There is no out-of-band scaler file to ship or
distribute.

Fresh exports add a `run_provenance` block to the sidecar. It records
the exporter entrypoint, CLI arguments, checkpoint input, and ONNX /
sidecar output paths so refreshed model artifacts can be traced without
reading local shell history.

Effective topology:

```text
features [N, 6]
   |
   Sub  <- mean   ([6] constant)
   |
   Div  <- std    ([6] constant)
   |
   Linear(6,16) → ReLU → Linear(16,8) → ReLU → Linear(8,1)
   |
   Squeeze(-1) -> vmaf [N]
```

## Training data

- Netflix Public Dataset (9 sources × encodings — local extract).
- KoNViD-1k (5-fold extract; CC BY 4.0; not redistributed).
- BVI-DVC subsets A + B + C + D (full coverage).

All combined into `runs/full_features_4corpus.parquet` (330 499
frame-rows × 22 FULL_FEATURES + `vmaf` teacher score from
`vmaf_v0.6.1`). The 4-corpus union is what we fit the StandardScaler
and the MLP on for the production export. LOSO + 5-fold are the
validation methodology, not the deployment recipe.

The shipped weights were retrained on the full 4-corpus union after
the 3-corpus sweep validated the canonical-6 + `lr=1e-3` + 90ep
configuration (Phase-3 chain → ADR-0244). Adding the BVI-DVC A + B
subsets brings the row count from 305 795 to 330 499 (+24 704 rows,
+8.1 %) and keeps train PLCC at 0.9999 / RMSE 0.153.

## Validation

Per the Phase-3 chain (Research-0027 → 0028 → 0029 → 0030):

| Methodology | PLCC | SROCC | Notes |
| --- | --- | --- | --- |
| Netflix LOSO (9 folds × 5 seeds) | **0.9978 ± 0.0021** | 0.9959 ± 0.0027 | +0.005–0.018 over Subset-B baseline |
| KoNViD 5-fold | **0.9998** | 0.9989 | corpus-portability gate (Phase-3b) |

The `min-PLCC = 0.97` ship gate runs in
`ai/scripts/validate_vmaf_tiny_v2.py` against
`runs/full_features_netflix.parquet` first 100 rows; refuses to
exit-0 below the gate. Pass `--out-json` when preserving promotion
evidence; the JSON report includes ADR-0661 `run_provenance` for the
ONNX, parquet, parsed gate arguments, and report path.

## Usage — CLI

```bash
# Use vmaf_tiny_v2 instead of the classic SVM regressor.
vmaf -r ref.yuv -d dis.yuv -w 1920 -h 1080 -p 420 -b 8 \
     --tiny-model model/tiny/vmaf_tiny_v2.onnx \
     --tiny-device auto
```

`--tiny-device auto` walks `cuda → openvino → rocm → cpu`. The model
is so small (<2 KB) that the dispatch overhead dominates wall-clock
on every device; CPU is usually the fastest path.

## Usage — Python (ONNX Runtime)

For research workflows that already have the canonical-6 features in
hand (e.g. from `runs/full_features_*.parquet`):

```python
import numpy as np
import onnxruntime as ort
import pandas as pd

sess = ort.InferenceSession("model/tiny/vmaf_tiny_v2.onnx",
                            providers=["CPUExecutionProvider"])

df = pd.read_parquet("runs/full_features_netflix.parquet").head(100)
features = df[
    ["adm2", "vif_scale0", "vif_scale1",
     "vif_scale2", "vif_scale3", "motion2"]
].to_numpy(dtype=np.float32)

(vmaf,) = sess.run(None, {"features": features})
print(vmaf[:5])  # -> per-frame VMAF estimates
```

## Reproducer

```bash
# 1. Train on the 4-corpus parquet (~12 min CPU on a typical dev box).
python3 ai/scripts/train_vmaf_tiny_v2.py \
    --parquet runs/full_features_4corpus.parquet \
    --out-ckpt /tmp/vmaf_tiny_v2.pt \
    --out-stats /tmp/vmaf_tiny_v2_stats.json

# 2. Export to ONNX with bundled scaler stats.
python3 ai/scripts/export_vmaf_tiny_v2.py \
    --ckpt /tmp/vmaf_tiny_v2.pt \
    --out-onnx model/tiny/vmaf_tiny_v2.onnx \
    --out-sidecar model/tiny/vmaf_tiny_v2.json

# 3. Validate (PLCC must be >= 0.97 on the Netflix slice).
python3 ai/scripts/validate_vmaf_tiny_v2.py \
    --onnx model/tiny/vmaf_tiny_v2.onnx \
    --parquet runs/full_features_netflix.parquet \
    --rows 100 --min-plcc 0.97 \
    --out-json runs/vmaf_tiny_v2_validate.json
```

The training stats JSON includes ADR-0661 `run_provenance` with the trainer
entrypoint, argv, parsed hyperparameters, parquet input, checkpoint target, and
stats target. Keep that block with any refreshed stats used for export.

## Limitations

- The model fuses six **already-extracted** features — it is *not* a
  pixel-input quality model. To use it from raw YUV, the feature
  extraction stage runs first (the regular libvmaf path).
- Trained on SDR content. HDR coverage is out of scope until the
  upstream HDR feature extractors land.
- The 4-corpus parquet uses `vmaf_v0.6.1` as the teacher score; v2
  cannot exceed `vmaf_v0.6.1` in absolute correctness — it
  approximates the SVM with a much smaller MLP.
- Bit-exactness across CPU/GPU execution providers is not guaranteed
  (ADR-0042 / ADR-0119 — places=4 tolerance applies to tiny-AI
  models too).

## See also

- [Phase-3 research chain](../../research/0027-phase2-feature-importance.md)
- [Phase-3 subset sweep](../../research/0028-phase3-subset-sweep.md)
- [Phase-3b StandardScaler results](../../research/0029-phase3b-standardscaler-results.md)
- [Phase-3b multi-seed validation](../../research/0030-phase3b-multiseed-validation.md)
- [ADR-0244 — vmaf_tiny_v2 ship decision](../../adr/0244-vmaf-tiny-v2.md)

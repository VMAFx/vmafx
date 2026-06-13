<!-- markdownlint-disable MD060 -->
# vmaf_tiny_v3 — wider/deeper VMAF feature-fusion estimator

`vmaf_tiny_v3` is a tiny multi-layer perceptron that predicts a VMAF
score from the same six classic libvmaf features as
[`vmaf_tiny_v2`](vmaf_tiny_v2.md) (`adm2`, `vif_scale0..3`, `motion2`
— the canonical-6 set used by `vmaf_v0.6.1`). It carries roughly 3x
the hidden capacity of v2 (`mlp_medium` = 6 → 32 → 16 → 1, ~769
params vs v2's 257) to test whether extra capacity buys headroom
over v2's PLCC = 0.9978 ± 0.0021 baseline on Netflix LOSO.

> **Production default stays `vmaf_tiny_v2`.** v3 is shipped
> alongside v2 as a higher-PLCC option, not a replacement: the
> measured LOSO win is small (+0.0008 PLCC mean, lower variance),
> the parameter count triples, and the on-disk ONNX size grows from
> 2 446 to 4 496 bytes. Pick v3 when the extra PLCC is worth the
> extra capacity; pick v2 when you want the smallest possible
> bundle.

## What the output means

A single scalar per frame on the same 0–100 VMAF scale as the
classic SVM regressor and as v2. Identical interpretation table:

| Value | Interpretation |
| --- | --- |
| **100** | Perceptually identical to the reference |
| **80–95** | High-quality encode |
| **60–80** | Visible compression artifacts |
| **< 60** | Heavy degradation |

## Shipped checkpoint

| Field | Value |
| --- | --- |
| Model name | `vmaf_tiny_v3` |
| Location | `model/tiny/vmaf_tiny_v3.onnx` |
| Architecture | `mlp_medium` — Linear(6, 32) → ReLU → Linear(32, 16) → ReLU → Linear(16, 1), 769 params |
| Input | `features` — float32 `[N, 6]`, dynamic batch |
| Feature order | `adm2, vif_scale0, vif_scale1, vif_scale2, vif_scale3, motion2` |
| Output | `vmaf` — float32 `[N]` |
| ONNX opset | 17 |
| Quantisation | fp32 + dynamic-PTQ int8 sidecar (ADR-0275) |
| License | BSD-3-Clause-Plus-Patent |
| Registry entry | `vmaf_tiny_v3` in `model/tiny/registry.json` |
| Sidecar | `model/tiny/vmaf_tiny_v3.json` |
| Exporter | `ai/scripts/export_vmaf_tiny_v3.py` |
| Trainer | `ai/scripts/train_vmaf_tiny_v3.py` |

The graph bakes the StandardScaler `(mean, std)` from the training
set as Constant nodes that run *before* the MLP — exactly the same
runtime contract as v2. There is no out-of-band scaler file to ship.

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
   Linear(6,32) → ReLU → Linear(32,16) → ReLU → Linear(16,1)
   |
   Squeeze(-1) -> vmaf [N]
```

## Training data

Identical to v2: `runs/full_features_4corpus.parquet` (Netflix Public

- KoNViD-1k 5-fold + BVI-DVC A + B + C + D, 330 499 frame-rows × 22
FULL_FEATURES + `vmaf` teacher score from `vmaf_v0.6.1`). The
StandardScaler is fit on the 4-corpus union and baked into the
exported ONNX as Constant nodes.

## Validation

| Methodology | v2 (mlp_small, 257 params) | v3 (mlp_medium, 769 params) | Δ |
| --- | ---:| ---:| ---:|
| Netflix LOSO (9 folds, seed=0) mean PLCC | 0.9978 ± 0.0021 | **0.9986 ± 0.0015** | +0.0008 |
| Netflix LOSO mean SROCC | 0.9959 ± 0.0027 | **0.9977 ± 0.0017** | +0.0018 |
| Netflix LOSO mean RMSE | — | 1.256 ± 0.604 | — |
| 5000-row Netflix smoke PLCC | 0.9998 | **1.0000** | +0.0002 |
| Train-set RMSE (4-corpus) | 0.153 | **0.112** | -0.041 |
| Parameter count | 257 | 769 | ×3.0 |
| ONNX file size | 2 446 B | 4 496 B | +2 050 B (×1.84) |

LOSO methodology: for each of the 9 Netflix sources, the model is
trained from scratch on the union of the other 8 sources (with
StandardScaler fit on those 8) and evaluated on the held-out source.
Recipe held identical between v2 and v3 — only the architecture
function differs. Per-fold metrics are pinned in
`runs/vmaf_tiny_v3_loso_metrics.json`.

Fresh LOSO reports from `ai/scripts/eval_loso_vmaf_tiny_v3.py` include a
`run_provenance` block with the evaluation entrypoint, argv, parsed
hyperparameters, feature parquet input, and report target path. This lets
refreshed v3 numbers be compared against older runs without guessing which
local feature table produced them.

The PLCC delta is small in absolute terms but the variance shrinks
~30 % — v3 is a more *consistent* estimator across hold-out clips.
SROCC also improves by 0.0018 mean, suggesting the ranking signal is
slightly cleaner with the extra capacity.

The `min-PLCC = 0.97` ship gate runs in
`ai/scripts/validate_vmaf_tiny_v3.py` against
`runs/full_features_netflix.parquet`; refuses to exit-0 below the
gate. Pass `--out-json` when preserving promotion evidence; the JSON
report includes ADR-0661 `run_provenance` for the ONNX, parquet, optional
v2 comparison model, parsed gate arguments, and report path.

## Usage — CLI

```bash
# Use vmaf_tiny_v3 instead of the default v2 / classic SVM.
vmaf -r ref.yuv -d dis.yuv -w 1920 -h 1080 -p 420 -b 8 \
     --tiny-model model/tiny/vmaf_tiny_v3.onnx \
     --tiny-device auto
```

`--tiny-device auto` walks `cuda → openvino → rocm → cpu`. As with
v2 the model is small enough (~4 KB) that dispatch overhead
dominates; CPU is usually the fastest path.

## Usage — Python (ONNX Runtime)

```python
import numpy as np
import onnxruntime as ort
import pandas as pd

sess = ort.InferenceSession("model/tiny/vmaf_tiny_v3.onnx",
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
# 1. Train on the 4-corpus parquet (~30 s wall on a 16-thread CPU).
python3 ai/scripts/train_vmaf_tiny_v3.py \
    --parquet runs/full_features_4corpus.parquet \
    --out-ckpt /tmp/vmaf_tiny_v3.pt \
    --out-stats /tmp/vmaf_tiny_v3_stats.json

# 2. Export to ONNX with bundled scaler stats.
python3 ai/scripts/export_vmaf_tiny_v3.py \
    --ckpt /tmp/vmaf_tiny_v3.pt \
    --out-onnx model/tiny/vmaf_tiny_v3.onnx \
    --out-sidecar model/tiny/vmaf_tiny_v3.json

# 3. Validate (PLCC must be >= 0.97 on the Netflix slice).
python3 ai/scripts/validate_vmaf_tiny_v3.py \
    --onnx model/tiny/vmaf_tiny_v3.onnx \
    --parquet runs/full_features_netflix.parquet \
    --rows 5000 --min-plcc 0.97 \
    --v2-onnx model/tiny/vmaf_tiny_v2.onnx \
    --out-json runs/vmaf_tiny_v3_validate.json

# 4. LOSO comparison vs v2.
python3 ai/scripts/eval_loso_vmaf_tiny_v3.py \
    --parquet runs/full_features_netflix.parquet \
    --out-json runs/vmaf_tiny_v3_loso_metrics.json
```

The training stats JSON includes ADR-0661 `run_provenance` with the trainer
entrypoint, argv, parsed hyperparameters, parquet input, checkpoint target, and
stats target. Keep that block with any refreshed stats used for export.

## Choosing between v2 and v3

- **Default to v2.** Smaller bundle (2 446 B vs 4 496 B), validated
  Phase-3 chain, +0.005–0.018 PLCC over the upstream SVM. v2 is the
  baseline for 99 % of users.
- **Use v3 when:** you need the lowest-variance VMAF estimator across
  diverse content (the LOSO std shrinks 30 %), or when the upstream
  pipeline is already paying ONNX-Runtime dispatch cost and the
  extra +0.0008 PLCC mean is worth the +2 KB.
- **Don't use v3 when:** disk / network footprint matters (e.g.
  embedded deploys, very-many-model bundles), or when the
  measurement target is upstream-comparable metrics — v2 is the
  cited baseline in the Phase-3 chain.

## Quantisation (dynamic-PTQ int8 sidecar — ADR-0275)

A dynamic-PTQ int8 sibling lives at
`model/tiny/vmaf_tiny_v3.int8.onnx`, produced by
`ai/scripts/ptq_dynamic.py`. The runtime redirect in
`vmaf_dnn_session_open` (ADR-0174) loads the int8 sidecar when the
registry entry's `quant_mode != "fp32"`; the fp32 file stays on disk
as the regression baseline.

| Field | Value |
| --- | --- |
| Quant mode | `dynamic` (weights-only int8; activations quantised at runtime) |
| Sidecar file | `model/tiny/vmaf_tiny_v3.int8.onnx` |
| sha256 (int8) | `b4bdbb353b1b395adb22b77e890117228de1cfd12b94cb46fd0173c2fd12a343` |
| File size | 4 267 B int8 vs 4 496 B fp32 (×0.95) — tiny MLP, weights are already a small fraction of the graph |
| PLCC drop (int8 vs fp32) | 0.000120 (Netflix features parquet, ~11k rows) |
| Budget | 0.01 (per ADR-0174 / ADR-0173 default) |
| CI gate | `ai-quant-accuracy` step in `Tiny AI` job |

The 4-KB MLP graph is dominated by op metadata and Constant scaler
nodes; only the three Linear weight tensors quantise. The size delta
is therefore small but the PLCC drop is still well inside budget.
v4's `mlp_large` carries enough weight mass that its int8 sidecar
shrinks ~45% — see [`vmaf_tiny_v4`](vmaf_tiny_v4.md).

```bash
# Reproduce
python ai/scripts/ptq_dynamic.py model/tiny/vmaf_tiny_v3.onnx
python ai/scripts/measure_quant_drop.py model/tiny/vmaf_tiny_v3.onnx
# -> [PASS] vmaf_tiny_v3   mode=dynamic PLCC=0.999880  drop=0.000120  budget=0.0100
```

## Limitations

- The model fuses six **already-extracted** features — it is *not* a
  pixel-input quality model. To use it from raw YUV, the feature
  extraction stage runs first (the regular libvmaf path).
- Trained on SDR content. HDR coverage is out of scope until the
  upstream HDR feature extractors land.
- The 4-corpus parquet uses `vmaf_v0.6.1` as the teacher score; v3
  cannot exceed `vmaf_v0.6.1` in absolute correctness — it
  approximates the SVM with a slightly larger MLP.
- Bit-exactness across CPU/GPU execution providers is not guaranteed
  (ADR-0042 / ADR-0119 — places=4 tolerance applies to tiny-AI
  models too).
- Single-seed LOSO. v2's published 0.9978 was averaged over 5 seeds;
  v3's 0.9986 is the seed=0 number. A multi-seed v3 sweep is
  follow-up scope.

## See also

- [`vmaf_tiny_v2` model card](vmaf_tiny_v2.md) — production default
- [ADR-0241 — vmaf_tiny_v3 ship decision](../../adr/0241-vmaf-tiny-v3-mlp-medium.md)
- [Research-0046 — v2-vs-v3 mlp-medium evaluation](../../research/0046-vmaf-tiny-v3-mlp-medium-evaluation.md)
- [Phase-3 research chain](../../research/0027-phase2-feature-importance.md)

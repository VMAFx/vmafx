# `vmaf_tiny_v1` — legacy mlp_small VMAF fusion regressor

> **Status — Superseded.** `vmaf_tiny_v1` is retained for LOSO-eval
> baselines and quantization-epsilon regression fixtures only.
> For production use, prefer [`vmaf_tiny_v2`](vmaf_tiny_v2.md) or later.
> See [ADR-0244](../../adr/0244-vmaf-tiny-v2.md).

`vmaf_tiny_v1` is the original tiny MLP fusion head trained over the
canonical-6 libvmaf features (`adm2`, `vif_scale0..3`, `motion2`).
It carries `mlp_small` architecture (Linear 6→16→8→1) with a separately
shipped `StandardScaler` file, matching the early Phase-1 export recipe
before scaler-baking (ADR-0244) was introduced. It is the *single-split
LOSO baseline* referenced in
[`docs/ai/loso-eval.md`](../loso-eval.md),
[`docs/ai/quant-eps.md`](../quant-eps.md), and
[`docs/ai/quantization.md`](../quantization.md).

## Shipped checkpoint

| Field | Value |
| --- | --- |
| Model id | `vmaf_tiny_v1` |
| Location | `model/tiny/vmaf_tiny_v1.onnx` |
| Architecture | `mlp_small` — Linear(6, 16) → ReLU → Linear(16, 8) → ReLU → Linear(8, 1) |
| Trainable parameters | **257** |
| Input | `features` — float32 `[N, 6]`, dynamic batch |
| Feature order | `adm2, vif_scale0, vif_scale1, vif_scale2, vif_scale3, motion2` |
| Output | `vmaf` — float32 `[N]` |
| ONNX opset | 17 |
| License | BSD-3-Clause-Plus-Patent |
| Registry entry | `vmaf_tiny_v1` in `model/tiny/registry.json` |
| SHA-256 | `d30201dfa8a0cb1d6d5bbe342b0f9049e40bf86e57b2e3b14cbfcade9231e7a6` |

> **Note on external data.** The ONNX file uses the ONNX external-data
> format; the weight tensor lives alongside the `.onnx` file as
> `vmaf_tiny_v1.onnx.data`. Both files must be present for the model
> to load. This was repaired in PR #296 / PR #174 after an
> external-data filename mismatch was found in the initial commit.

## Training corpus

The v1 checkpoint was trained on the **Netflix Public Dataset** only
(9 reference sources × multiple encodings — local extract; not
redistributed in-tree). The teacher score is `vmaf_v0.6.1` (the classic
SVM). This single-corpus baseline is what `docs/ai/loso-eval.md`
references when reporting LOSO scores for the v1 architecture.

No KoNViD-1k or BVI-DVC rows were included at v1 training time. This
is the key difference vs v2's 4-corpus union; v1 generalises less well
to UGC content.

## Validation

v1 has **not** been re-evaluated on the full Phase-3 chain. The relevant
LOSO figures appear in `docs/ai/loso-eval.md` as the single-split v1
baseline; they are not re-quoted here to avoid drift. For current
production PLCC / SROCC / RMSE numbers, see
[`vmaf_tiny_v2.md`](vmaf_tiny_v2.md).

## When to use this model

| Use case | Recommendation |
| --- | --- |
| Production VMAF fusion | Use [`vmaf_tiny_v2`](vmaf_tiny_v2.md) |
| LOSO-eval historical baseline | `vmaf_tiny_v1` — required fixture |
| Quantization-epsilon regression | `vmaf_tiny_v1` — required fixture (ADR-0203) |
| Capacity comparison | Pair with [`vmaf_tiny_v1_medium`](vmaf_tiny_v1_medium.md) |

## Known limitations

- Trained on Netflix Public Dataset only — limited generalization to
  UGC content relative to the 4-corpus v2 checkpoint.
- StandardScaler is **not** baked into the ONNX graph (unlike v2+).
  A separate scaler artefact is required for standalone ORT inference
  outside the libvmaf DNN integration path; the C integration path
  handles this transparently.
- Uses ONNX external-data format; both `vmaf_tiny_v1.onnx` and
  `vmaf_tiny_v1.onnx.data` must be co-located.
- Superseded by `vmaf_tiny_v2` for accuracy (PLCC improvement of
  0.005–0.018 across the validation chain). Do not use v1 in new
  pipelines.

## See also

- [`vmaf_tiny_v1_medium.md`](vmaf_tiny_v1_medium.md) — the `mlp_medium`
  sibling trained under the same Phase-1 recipe.
- [`vmaf_tiny_v2.md`](vmaf_tiny_v2.md) — the production-default successor.
- [`docs/ai/loso-eval.md`](../loso-eval.md) — LOSO methodology; cites v1
  as the single-split baseline.
- [`docs/ai/quantization.md`](../quantization.md) — quantization-epsilon
  analysis against v1.
- [`docs/ai/quant-eps.md`](../quant-eps.md) — quant-eps regression fixture
  uses v1.
- [ADR-0203](../../adr/0203-vmaf-tiny-v1-quant-eps.md) — v1 as quant-eps
  fixture decision.
- [ADR-0244](../../adr/0244-vmaf-tiny-v2.md) — v2 ship decision (supersedes
  v1 as default).
- [ADR-0042](../../adr/0042-tinyai-docs-required-per-pr.md) — tiny-AI
  doc-substance rule this card satisfies.

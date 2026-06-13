# `fr_regressor_v2_ensemble_v1` — probabilistic FR regressor (deep-ensemble + conformal)

`fr_regressor_v2_ensemble_v1` is a **probabilistic** successor to the
codec-aware `fr_regressor_v2` (parent: [ADR-0272](../../adr/0272-fr-regressor-v2-codec-aware-scaffold.md))
that surfaces a **prediction interval** around each VMAF score instead of
just a point estimate. Producers can ask risk-tolerant questions of the
form _"give me the CRF where the **lower** bound of the 95 % interval is
still ≥ 92"_ — driving the new `vmaf-tune --quality-confidence` flag
(planned, see [ADR-0237](../../adr/0237-quality-aware-encode-automation.md)).

> **Status — smoke placeholder for the RC; production flip deferred to
> the one-shot post-RC retrain ([ADR-1105](../../adr/1105-ensemble-v2-prod-flip-deferred-oneshot-retrain.md)).**
> The five `fr_regressor_v2_ensemble_v1_seed{0..4}` rows in
> `model/tiny/registry.json` currently carry `smoke: true`. The
> ADR-0321 production flip (2026-05-06) shipped LOSO-validated weights
> (mean PLCC 0.997, spread 0.001 per `runs/ensemble_v2_real/PROMOTE.json`)
> trained against a codec one-hot of width 14. `codec_vocab` was later
> trimmed to 6, which made those weights' input dimension stale; PR #865
> regenerated the on-disk ONNX at the correct width but in `--smoke`
> mode (1 epoch, synthetic corpus) to keep the load path working. Those
> smoke weights are placeholders, not a production fit. Re-establishing
> production at `codec_vocab=6` requires re-running
> `export_ensemble_v2_seeds.py`, which is part of the locked one-shot
> post-RC retrain (the ensemble is in scope). Until then,
> `test_fr_regressor_v2_ensemble_seed_rows_are_production` is marked
> `xfail(strict=True)`; it auto-fails the suite the moment the retrain
> lands real weights (`smoke: false` + matching sidecar sha), forcing
> removal of the marker. The per-seed sidecars
> (`fr_regressor_v2_ensemble_v1_seed{N}.json`) still describe the older
> production weights and retain their PROMOTE provenance; the one-shot
> retrain regenerates ONNX + sidecars together. See
> [ADR-0303](../../adr/0303-fr-regressor-v2-ensemble-prod-flip.md)
> (gate definition),
> [ADR-0309](../../adr/0309-fr-regressor-v2-ensemble-real-corpus-retrain.md)
> (separate-PR rule),
> [ADR-0319](../../adr/0319-ensemble-loso-trainer-real-impl.md) (LOSO
> trainer),
> [ADR-0321](../../adr/0321-fr-regressor-v2-ensemble-full-prod-flip.md)
> (the original production flip), and
> [ADR-1105](../../adr/1105-ensemble-v2-prod-flip-deferred-oneshot-retrain.md)
> (RC deferral). The scaffold-era ADR-0279 entry point is preserved
> for history.

## What the output means

For one (frame, codec) pair the ensemble emits four numbers:

| Field   | Meaning                                                           |
| ------- | ----------------------------------------------------------------- |
| `mu`    | Mean of the 5 member predictions — the point estimate (≈ v2).     |
| `sigma` | Sample std (ddof=1) across members — epistemic uncertainty proxy. |
| `lower` | Lower interval edge (default 95 % nominal coverage).              |
| `upper` | Upper interval edge.                                              |

Two interval-construction modes are wired in (the manifest pins which
one is active for the shipped checkpoint):

1. **`ensemble`** (default) — Gaussian assumption. With `z_α/2 = 1.96`
   for 95 % coverage:

   ```text
   lower = mu - 1.96 * sigma
   upper = mu + 1.96 * sigma
   ```

2. **`ensemble+conformal`** — split-conformal calibration (Romano et
   al. 2019, Vovk-style). The trainer holds out a calibration fraction
   of the corpus (default `--conformal-calibration-frac 0.2`), computes
   the standardised residual `|y - mu| / sigma` on the held-out rows,
   and stores the empirical (1 − α) quantile as
   `confidence.conformal_q_residual` in the manifest. Inference:

   ```text
   lower = mu - q * sigma
   upper = mu + q * sigma
   ```

   Marginal coverage is provably ≥ 1 − α on exchangeable data
   regardless of whether `(mu, sigma)` is well-calibrated (Vovk 2005,
   Lei et al. 2018). Conformal trades a calibration-set training cost
   for a coverage _guarantee_ — preferred over the Gaussian assumption
   when the empirical coverage on the held-out set falls below
   nominal.

## Architecture

Five copies of `FRRegressor(in_features=6, hidden=64, depth=2,
dropout=0.1, num_codecs=NUM_CODECS)` from
[`ai/src/vmaf_train/models/fr_regressor.py`](../../../ai/src/vmaf_train/models/fr_regressor.py)
trained under seeds `[base_seed, base_seed+1, …, base_seed+4]`. Each
member is the same code path as the v2 deterministic checkpoint —
the ensemble is a _training-time_ and _inference-time_ aggregation
only; no architecture change. Member ONNX files are exported
individually and referenced from the ensemble manifest.

ONNX I/O contract (per member, mirrors the LPIPS-Sq two-input
precedent — [ADR-0040](../../adr/0040-dnn-session-multi-input-api.md),
[ADR-0041](../../adr/0041-lpips-sq-extractor.md)):

```text
Inputs:
  features:     float32 [N, 6]            # canonical-6, StandardScaler-normalised
  codec_onehot: float32 [N, NUM_CODECS]   # one-hot codec id (CODEC_VOCAB pinned)
Output:
  score:        float32 [N]               # VMAF MOS scalar
```

Opset 17, dynamic batch axis. Each member is ~3 KB graph + ~19 KB
external-data weights.

## Manifest layout (`fr_regressor_v2_ensemble_v1.json`)

```jsonc
{
  "id": "fr_regressor_v2_ensemble_v1",
  "kind": "fr_ensemble",
  "ensemble_size": 5,
  "members": [
    { "id": "fr_regressor_v2_ensemble_v1_seed0",
      "onnx": "fr_regressor_v2_ensemble_v1_seed0.onnx",
      "seed": 0, "sha256": "…" },
    /* … 4 more … */
  ],
  "feature_order": ["adm2", "vif_scale0", "vif_scale1",
                    "vif_scale2", "vif_scale3", "motion2"],
  "feature_mean": [/* 6 floats — applied at inference time */],
  "feature_std":  [/* 6 floats — applied at inference time */],
  "codec_vocab":  ["x264", "x265", "libsvtav1", "libvvenc",
                   "libvpx-vp9", "unknown"],
  "codec_vocab_version": 1,
  "confidence": {
    "method": "ensemble" | "ensemble+conformal",
    "nominal_coverage": 0.95,
    "gaussian_z": 1.959963984540054,
    "conformal_q_residual": null    /* set by trainer when --conformal-calibration-frac > 0 */
  },
  "smoke": true,
  "eval": { /* in-sample mu PLCC / RMSE / mean_sigma + cal summary */ }
}
```

Each ensemble member is also added to `model/tiny/registry.json` as a
`kind: "fr"` entry so the existing tiny-model loader / verifier can
SHA-256-check each member without a registry-schema bump. The manifest
is the higher-level entry point that wires the members into a single
ensemble identifier.

Fresh manifests written by `ai/scripts/train_fr_regressor_v2_ensemble.py`
include ADR-0661 `run_provenance`. That block records the trainer entrypoint,
argv, parsed training/export arguments, optional corpus parquet, member ONNX
outputs, registry target, and manifest path. The later production seed exporter
also records provenance in each per-seed sidecar, so the top-level manifest and
individual ONNX sidecars both carry replay context.

## How to query

The runtime contract is "run all 5 ONNX sessions on the same input,
aggregate". A reference implementation lives in
[`ai/scripts/eval_probabilistic_proxy.py`](../../../ai/scripts/eval_probabilistic_proxy.py)
(Python / onnxruntime); the C-side adapter that wires this into
`core/src/dnn/` is the T7-FR-REGRESSOR-V2-PROBABILISTIC follow-up.
When `--metrics-out` is used, the evaluator writes `run_provenance` alongside
the coverage metrics so stale probabilistic reports can be traced to the exact
ensemble manifest, optional held-out parquet, argv, and output path.

```python
import json, numpy as np, onnxruntime as ort

manifest = json.loads(open("model/tiny/fr_regressor_v2_ensemble_v1.json").read())
sessions = [ort.InferenceSession(f"model/tiny/{m['onnx']}",
                                 providers=["CPUExecutionProvider"])
            for m in manifest["members"]]

# features: (N, 6) raw canonical-6; codec_onehot: (N, NUM_CODECS)
mean = np.asarray(manifest["feature_mean"], dtype=np.float32)
std  = np.asarray(manifest["feature_std"],  dtype=np.float32)
x_norm = (features - mean) / np.where(std < 1e-8, 1.0, std)

preds = np.vstack([
    s.run(None, {"features": x_norm.astype(np.float32),
                 "codec_onehot": codec_onehot.astype(np.float32)})[0].reshape(-1)
    for s in sessions
])  # (5, N)
mu, sigma = preds.mean(axis=0), preds.std(axis=0, ddof=1)

q = manifest["confidence"].get("conformal_q_residual") or manifest["confidence"]["gaussian_z"]
lower, upper = mu - q * sigma, mu + q * sigma
```

The downstream `vmaf-tune --quality-confidence 0.95 --target 92` flag
returns the smallest CRF for which `lower(CRF) >= 92` — i.e. the
conservative encode that meets the quality target with ≥ 95 %
probability under the model's coverage guarantee.

## How to (re)train

Smoke (no real corpus):

```sh
python ai/scripts/train_fr_regressor_v2_ensemble.py --smoke
```

Smoke + conformal calibration:

```sh
python ai/scripts/train_fr_regressor_v2_ensemble.py --smoke \
    --conformal-calibration-frac 0.2 --nominal-coverage 0.95
```

Production (Phase A multi-codec parquet — gated on T7-FR-REGRESSOR-V2-PROBABILISTIC):

```sh
python ai/scripts/train_fr_regressor_v2_ensemble.py \
    --corpus runs/full_features_phase_a.parquet \
    --ensemble-size 5 --epochs 30 \
    --conformal-calibration-frac 0.2 --nominal-coverage 0.95
```

Evaluate empirical coverage on a held-out parquet:

```sh
python ai/scripts/eval_probabilistic_proxy.py \
    --manifest model/tiny/fr_regressor_v2_ensemble_v1.json \
    --parquet runs/full_features_phase_a_holdout.parquet
```

The eval reports mean PLCC, mean sigma, and **empirical coverage** at
50 / 80 / 95 % nominal levels — well-calibrated outputs match nominal
within sampling error. When the conformal scalar is present, an extra
row reports the conformal interval's empirical coverage (should be
`>= 1 - alpha` by construction).

## Why deep-ensemble + conformal

See [Research-0054](../../research/0067-fr-regressor-v2-probabilistic.md)
for the full audit (PR #354 Bucket #18, top-3 ranked) and the decision
matrix in [ADR-0279 § Alternatives considered](../../adr/0279-fr-regressor-v2-probabilistic.md).
Short version: deep ensembles dominate single-network alternatives
(MC-dropout, single-network heteroscedastic NLL) on calibration
quality, with the conformal layer giving a distribution-free coverage
guarantee at negligible inference-time cost.

## References

- Lakshminarayanan, Pritzel, Blundell (2017), _Simple and Scalable
  Predictive Uncertainty Estimation using Deep Ensembles_.
- Vovk, Gammerman, Shafer (2005), _Algorithmic Learning in a Random
  World_ (the conformal-prediction reference).
- Romano, Patterson, Candès (2019), _Conformalized Quantile
  Regression_ — the "normalised residual" scheme used here.
- [ADR-0272](../../adr/0272-fr-regressor-v2-codec-aware-scaffold.md) —
  parent v2 deterministic scaffold.
- [ADR-0279](../../adr/0279-fr-regressor-v2-probabilistic.md) — this
  ADR (probabilistic head + conformal calibration).
- [ADR-0237](../../adr/0237-quality-aware-encode-automation.md) —
  vmaf-tune Phase A; the `--quality-confidence` consumer flag.
- [ADR-0040](../../adr/0040-dnn-session-multi-input-api.md),
  [ADR-0041](../../adr/0041-lpips-sq-extractor.md) — multi-input
  ONNX precedent the v2 ensemble member graph follows.

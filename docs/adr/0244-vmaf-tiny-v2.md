<!-- markdownlint-disable MD013 MD060 -->
# ADR-0244: vmaf_tiny_v2 — canonical-6 + StandardScaler tiny VMAF MLP

- **Status**: Accepted
- **Date**: 2026-04-29
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, dnn, tiny-ai, model, registry, fork-local

## Context

The shipped `vmaf_tiny_v1.onnx` was trained on the Netflix corpus alone with a single train/val split (`val=Tennis`); it had no scaler stats baked in and no end-to-end provenance to the FULL-feature parquet that the Phase-3 research chain produced. The Phase-3 chain (Research-0027 → 0028 → 0029 → 0030) validated a concrete configuration on Netflix LOSO + KoNViD 5-fold:

- **Architecture**: `mlp_small` (6 → 16 → 8 → 1, ~257 params). Phase-3d's arch sweep was inconclusive against `mlp_medium`, so the small variant remains the v2 baseline.
- **Features**: `canonical-6` = (`adm2`, `vif_scale0`, `vif_scale1`, `vif_scale2`, `vif_scale3`, `motion2`).
- **Preprocessing**: per-fold StandardScaler (fit on train, applied to val).
- **Optimiser**: Adam @ `lr=1e-3`, MSE loss, 90 epochs, batch_size 256.
- **Validated PLCC**: `0.9978 ± 0.0021` Netflix LOSO, `0.9998` KoNViD 5-fold; +0.005–0.018 PLCC over the prior Subset-B baseline.

The v2 model needs to ship these gains, and it needs to ship them in a way the runtime can consume without requiring the caller to know the scaler stats out-of-band.

## Decision

We ship `vmaf_tiny_v2.onnx` with the validated Phase-3 configuration and bake the StandardScaler `(mean, std)` directly into the ONNX graph as `Sub` + `Div` Constant nodes that run before the MLP. The runtime feeds raw canonical-6 feature values; the trust-root sha256 covers the calibration values too. opset 17 (matches `learned_filter_v1`, `nr_metric_v1`, `fastdvdnet_pre`). For the production export we fit the scaler on the **full 3-corpus parquet** (Netflix + KoNViD + BVI-DVC D+C) — Phase-3 LOSO + 5-fold are the validation methodology; the shipped weights are trained on the union for maximum corpus coverage. Registered as `vmaf_tiny_v2` (kind `fr`) in `model/tiny/registry.json` with `quant_mode: fp32` and `smoke: false`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Bundled scaler in the ONNX graph (chosen)** | Single-file deploy; trust-root sha256 covers calibration; runtime does no preprocessing | One-time export-time wrapping cost | Correct — matches the bundled-scaler design rule; runtime contract stays "feed features, read VMAF" |
| Sidecar JSON `input_mean` / `input_std` | Smaller ONNX; reuses the existing sidecar layout | Two-file trust contract; runtime needs preprocessing path; drift risk between ONNX and sidecar | Rejected — splits the trust root, adds runtime complexity for no win |
| `mlp_medium` (Phase-3d) | Slightly higher capacity ceiling | Phase-3d arch sweep was inconclusive; 4× param count for no measurable PLCC gain on the validation folds | Rejected — Phase-3d did not produce a positive signal |
| LOSO-final fold weights | Bit-equal to one of the validation folds | Drops 11 % of training data per fold; production deploy benefits from the union | Rejected — LOSO is a methodology, not a deployment recipe |
| Subset-B feature set | Different feature mix tested in Phase-3 | Failed Gate 2 corpus portability (KoNViD + BVI-DVC) — canonical-6 ships positive across all three corpora | Rejected — corpus portability is non-negotiable |
| Re-train on Netflix-only | Bit-equal to v1 methodology | Throws away KoNViD + BVI-DVC coverage | Rejected — the multi-corpus parquet is precisely the asset Phase-2 produced |

## Consequences

- **Positive**: ships a tiny VMAF estimator (~1 KB ONNX) with validated +0.005–0.018 PLCC over the prior Subset-B baseline; runtime needs no out-of-band calibration.
- **Negative**: introduces a second tiny-AI scoring model in the registry — the registry trust contract grows by one entry, and the docs / state matrix track v1 + v2 in parallel until v1 is retired.
- **Neutral / follow-ups**: `--tiny-model` default in `docs/ai/inference.md` flips to `vmaf_tiny_v2`; v1 stays on disk as a regression baseline. PTQ for v2 is deferred (the model is already <2 KB; 8-bit quantisation has no shipping payoff).

## References

- Research-0027 — feature importance (Phase-2)
- Research-0028 — Phase-3 subset sweep (canonical6 vs A vs B vs C)
- Research-0029 — Phase-3b StandardScaler results
- Research-0030 — Phase-3b multi-seed validation
- Trainer: [`ai/scripts/train_vmaf_tiny_v2.py`](../../ai/scripts/train_vmaf_tiny_v2.py)
- Exporter: [`ai/scripts/export_vmaf_tiny_v2.py`](../../ai/scripts/export_vmaf_tiny_v2.py)
- Source: `req` (user-provided spec — paraphrased: "Build + ship `vmaf_tiny_v2.onnx` end-to-end on the validated Phase-3 chain configuration; bundled scaler stats in the graph per the documented design rule.")

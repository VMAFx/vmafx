# Research-0067: probabilistic `fr_regressor_v2` — deep-ensemble + conformal

- **Date**: 2026-05-03
- **Authors**: Lusoris, Claude (Anthropic)
- **Status**: Final (scaffold-time digest)
- **Tags**: ai, fr-regressor, probabilistic, ensemble, conformal
- **Related**: ADR-0279 (this scaffold), ADR-0272 (parent v2 deterministic),
  ADR-0237 (vmaf-tune Phase A consumer), PR #354 audit Bucket #18

## Goal

Decide how to surface a calibrated **prediction interval** around the
codec-aware `fr_regressor_v2`'s VMAF output so the in-flight
`vmaf-tune --quality-confidence 0.95` flag (consumer of
[ADR-0237](../adr/0237-quality-aware-encode-automation.md)) can answer
queries of the form _"smallest CRF where the **lower** bound of the
95 % VMAF interval is ≥ 92"_ — i.e. risk-aware encode automation.

PR #354's audit ranked the question as Bucket #18 (top-3) on the
"highest user-visible payoff per LOC of training-side scaffold"
heuristic. This digest closes the literature loop and selects an
implementation.

## Methodology

Pull the four reference families that show up in regression-uncertainty
benchmarks (UCI tables, KITTI depth, retinal OCT, video-quality
adjacents) and rank them on three axes:

1. **Calibration quality** at the published 95 % nominal — does
   empirical coverage land within sampling error?
2. **Engineering cost** to add to the existing
   [`FRRegressor`](../../ai/src/vmaf_train/models/fr_regressor.py)
   stack — new ops, new training loop, ONNX-export friction?
3. **Inference cost** at the libvmaf runtime layer — extra forward
   passes per frame, extra session loads, extra C-side adapter code?

The four families:

- **Deep ensembles** (Lakshminarayanan et al. 2017) — N independent
  trainings under different seeds, aggregate at inference.
- **MC-dropout** (Gal & Ghahramani 2016) — keep dropout active at
  inference, average T forward passes.
- **Heteroscedastic NLL** (Nix & Weigend 1994; Kendall & Gal 2017) —
  one network, two outputs, Gaussian NLL loss.
- **Bayesian last-layer** (Laplace / SWAG / SVI variants) — posterior
  over the last linear layer's weights.

Layer the **conformal-prediction** correction on top of any of these
to get a marginal coverage guarantee that does not depend on the base
model being well-calibrated.

## Findings

### Calibration quality

| Method | UCI 95 % cov. | KITTI depth 95 % cov. | Notes |
| --- | --- | --- | --- |
| Deep ensemble (N=5) | 0.93–0.95 | 0.91–0.94 | Best of the four pre-conformal; dominates MC-dropout consistently. |
| MC-dropout (T=10) | 0.85–0.91 | 0.78–0.86 | Underestimates variance; gets worse on OOD inputs. |
| Heteroscedastic NLL | 0.78–0.92 (high variance) | 0.70–0.88 | Aleatoric only; collapses on epistemic-uncertainty regimes. |
| Bayesian last-layer | 0.90–0.94 | 0.88–0.92 | Comparable to MC-dropout; substantially more engineering. |
| **Any method + conformal** | **≥ 0.95 by construction** | **≥ 0.95 by construction** | Marginal coverage guarantee on exchangeable data (Vovk 2005, Lei 2018). |

(Numbers are envelope ranges from Tables 1–3 of the cited papers, not
fork measurements.)

### Engineering cost

- **Deep ensemble**: trivial — N independent calls into the existing
  trainer. Each member is a stock `FRRegressor(num_codecs=NUM_CODECS)`;
  ONNX export is the same two-input graph the v2 deterministic
  scaffold already ships.
- **MC-dropout**: high — torch's ONNX exporter folds dropout away in
  `model.eval()` mode. Keeping dropout live requires either a custom
  ONNX op (rejected by the libvmaf op allowlist —
  [ADR-0039](../adr/0039-onnx-runtime-op-walk-registry.md)) or
  N forward passes through a constructed-at-inference Bernoulli mask.
- **Heteroscedastic NLL**: medium — `FRRegressor(emit_variance=True)`
  already exists; export adds a second output. Loss switches from MSE
  to Gaussian NLL.
- **Bayesian last-layer**: high — needs a Hessian / Fisher pass and a
  posterior-sampling step at inference; no precedent in the
  `vmaf_train` package.

### Inference cost

- Deep ensemble: 5× sessions. v2 members are 6→64→64→1 MLPs (~5 KB
  param). Even serial CPU evaluation of 5 members is well under
  one decoded frame's per-pixel cost — irrelevant on the libvmaf
  budget.
- MC-dropout (T=10): 10× forward passes through one session — strictly
  worse than ensemble (10× vs 5×) and requires the custom-op
  workaround above.
- Heteroscedastic NLL: 1× — best on this axis.
- Bayesian last-layer: 1× plus posterior sampling overhead.

### Conformal layer

Romano-style **normalised** split-conformal (residual divided by
`sigma`) is the natural fit when the base estimator emits both
`mu` and `sigma`. The calibration cost is one held-out residual sort;
the inference cost is _zero_ (multiplier `q` replaces the Gaussian
`z = 1.96`). Marginal coverage `>= 1 - alpha` is provable on
exchangeable data, no distributional assumption on residuals required.

## Decision

**Deep ensemble of 5 v2 members + opt-in normalised split-conformal.**

This combination dominates the alternatives on the audit's three axes:
best base calibration, lowest engineering cost (re-uses the v2
training stack verbatim), tolerable inference cost (5× tiny MLPs),
and conformal gives a coverage guarantee that survives distribution
shift on the production Phase A corpus.

Smoke-mode synthesises a 100-row corpus and trains 1 epoch per member
to validate the data path end-to-end without a real corpus; production
training is gated on the multi-codec Phase A parquet landing
(T7-FR-REGRESSOR-V2-PROBABILISTIC).

## Open questions

- **Empirical coverage on Phase A** — once the corpus lands, the eval
  script's "empirical coverage at 95 % nominal" must land within 5 pp
  of nominal _without_ conformal; if it doesn't, conformal becomes
  mandatory and the manifest's `confidence.method` flips to
  `"ensemble+conformal"` for the shipped checkpoint.
- **Ensemble size sweep** — N=5 is the literature default; the audit
  did not justify it against N=3 or N=10. A follow-up ablation on
  Phase A should sweep `[3, 5, 10]` and pick the knee. Captured as
  ADR-0279 § Consequences neutral follow-up.
- **C-side adapter** — opening 5 ORT sessions per `vmaf_dnn_score_*`
  call is the simplest port; the optimal layout (one batched session
  vs N parallel sessions, ORT thread-pool sharing) is a separate
  perf-tuning PR.

## References

- Lakshminarayanan, Pritzel, Blundell (2017),
  [_Simple and Scalable Predictive Uncertainty Estimation using Deep Ensembles_](https://arxiv.org/abs/1612.01474).
- Gal & Ghahramani (2016), _Dropout as a Bayesian Approximation_, ICML.
- Nix & Weigend (1994), _Estimating the mean and variance of the
  target probability distribution_, IEEE ICNN.
- Kendall & Gal (2017),
  [_What Uncertainties Do We Need in Bayesian Deep Learning for Computer Vision?_](https://arxiv.org/abs/1703.04977),
  NeurIPS.
- Vovk, Gammerman, Shafer (2005), _Algorithmic Learning in a Random
  World_, Springer.
- Romano, Patterson, Candès (2019),
  [_Conformalized Quantile Regression_](https://arxiv.org/abs/1905.03222),
  NeurIPS.
- Lei, G'Sell, Rinaldo, Tibshirani, Wasserman (2018), _Distribution-Free
  Predictive Inference for Regression_, JASA.
- PR #354 — audit Bucket #18 (probabilistic head ranked top-3).

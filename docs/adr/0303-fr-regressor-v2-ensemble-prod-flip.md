<!-- markdownlint-disable MD049 MD060 -->
# ADR-0303: `fr_regressor_v2` ensemble — production flip trainer + CI gate

- **Status**: Accepted
- **Date**: 2026-05-05
- **Deciders**: Lusoris, Claude (Anthropic)
- **Companion research digest**: [Research-0075](../research/0075-fr-regressor-v2-ensemble-prod-flip.md)
- **Tags**: ai, fr-regressor, ensemble, probabilistic, loso, ci-gate, fork-local
- **Related**: [ADR-0291](0291-fr-regressor-v2-prod-ship.md) (v2 deterministic
  prod flip — defines the 0.95 LOSO PLCC ship gate),
  [ADR-0279](0279-fr-regressor-v2-probabilistic.md) (probabilistic head
  scaffold — deep-ensemble + conformal),
  [ADR-0235](0235-codec-aware-fr-regressor.md) (codec-aware decision +
  the 0.95 LOSO PLCC ship gate it inherits),
  [ADR-0237](0237-quality-aware-encode-automation.md) (Phase A consumer).

## Context

PR #372 shipped the **scaffold** for the `fr_regressor_v2_ensemble_v1`
deep ensemble — five `kind: "fr"` rows in `model/tiny/registry.json`
(`fr_regressor_v2_ensemble_v1_seed{0..4}`), each carrying `smoke: true`
and a 5 KB smoke ONNX. The scaffold wires the data path
(per-member sidecar, manifest JSON, `train_fr_regressor_v2_ensemble.py`
exporter) but the seed checkpoints are **not** trained on the real
Phase A hardware-encoder corpus yet — they are the same
under-trained smoke graphs as the original v2 scaffold from
[ADR-0272](0272-fr-regressor-v2-codec-aware-scaffold.md).

The deterministic [ADR-0291](0291-fr-regressor-v2-prod-ship.md) flip
proved the data path works end-to-end on the
`runs/phase_a/full_grid/per_frame_canonical6.jsonl` corpus
(33,840 per-frame canonical-6 rows × 9 Netflix sources × NVENC + QSV,
12-encoder vocab v2). What's missing is the LOSO trainer for the
**ensemble** that emits per-seed `loso_seed{N}.json` artefacts and a
CI gate script that promotes seeds from `smoke: true` to `smoke: false`
once they clear the production threshold.

The probabilistic head exists so the in-flight `vmaf-tune
--quality-confidence` flag (consumer of ADR-0237) can answer
risk-aware queries — _"smallest CRF such that the lower bound of the
95 % VMAF interval is ≥ 92"_. A deep ensemble gives that distribution
through five independent point estimates aggregated at inference; the
flip is only safe if the **ensemble** clears a tighter gate than any
single seed alone (otherwise the across-seed spread is misleading).

## Decision

We will land the LOSO trainer scaffold + CI gate in this PR (without
flipping the registry rows yet) so that a follow-up PR — gated on a
real-corpus LOSO run — can flip `smoke: true → false` for each seed
once it clears the gate. The actual ONNX swap and registry flip stay
out of scope here; only the trainer + gate ship.

The **production ship gate** for the ensemble is two-part and tighter
than ADR-0291's per-seed gate:

1. **Mean per-seed PLCC ≥ 0.95** — `mean_i(PLCC_i) ≥ 0.95` over the
   five seeds, where `PLCC_i` is the LOSO mean PLCC across the nine
   Netflix sources for seed `i`. This inherits ADR-0235 / ADR-0291's
   ship gate per member.
2. **Variance bound `max_i(PLCC_i) - min_i(PLCC_i) ≤ 0.005`** — the
   spread of per-seed LOSO PLCC across the ensemble must stay tight.
   A wider spread means the seeds disagree on which sources they
   generalise to, which would invalidate the ensemble-mean as an
   uncertainty estimator (the predictive distribution becomes
   bimodal-by-seed instead of bimodal-by-content, breaking the
   conformal calibration assumption).

A seed flips `smoke: true → false` **only after** it individually
clears `PLCC_i ≥ 0.95`. The ensemble-mean entry (if/when one is added
to the registry) flips **only after** all five seeds clear *and*
the variance bound holds.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **5-seed deep ensemble (chosen)** | Lakshminarayanan 2017 — strongest empirical calibration on regression benchmarks. ONNX op-allowlist clean (5 forward passes, no dropout-at-inference, no heteroscedastic NLL). Trivially parallel across seeds. Members are ~5 KB each — 25 KB total runtime cost. | 5× training wall time vs deterministic v2. Predictive variance scales with seed count; 5 is the smallest credible ensemble (Lakshminarayanan's paper shows diminishing returns past 5). | Selected — calibration quality + zero-friction ONNX export trumps the wall-time cost on a corpus that trains in <30 s per seed. |
| MC-dropout | Single trained model; T forward passes at inference for free. | Keeping dropout active at inference adds a `Dropout` op the libvmaf op allowlist currently rejects. Calibration on regression tasks is empirically worse than ensembles (Lakshminarayanan 2017 §5; Foong 2019 in-depth analysis). | Rejected — op-allowlist friction is unjustifiable when ensembles match the inference-time cost (5 forward passes ≈ T forward passes for T=5). |
| SWAG (Stochastic Weight Averaging — Gaussian) | Posterior over weights from SGD trajectory; one trained model + sampling at inference. | Sampling at inference adds either a runtime-side weight perturbation loop (new C code) or N pre-sampled checkpoints (same N× artefact cost as the ensemble, with lower calibration quality per Maddox 2019). The variance estimate depends on the SGD trajectory's last-K iterates — fragile to hyperparameter choices. | Rejected — same artefact cost as the ensemble for worse calibration on regression. |

## Consequences

- **Positive**: a clean trainer + gate path means the eventual
  registry flip is mechanical: run `train_fr_regressor_v2_ensemble_loso.py
  --seeds 0,1,2,3,4 --corpus runs/phase_a/full_grid/per_frame_canonical6.jsonl
  --out-dir runs/ensemble_loso/`, run `scripts/ci/ensemble_prod_gate.py`
  on the resulting `loso_seed{N}.json` files, flip the cleared seeds
  in the registry. No re-derivation needed at flip time.
- **Positive**: the variance bound (`max - min ≤ 0.005`) catches the
  pathological case where one seed wildly outperforms (or underperforms)
  the others — without it, mean PLCC ≥ 0.95 could mask a 0.99 + four
  0.94s situation that breaks the uncertainty estimate.
- **Negative**: the gate is strictly tighter than the deterministic
  v2 gate. A real corpus run might clear ADR-0291's gate but miss the
  variance bound here — that would force either re-seeding (cheap) or
  a wider tolerance (ADR change required, not silent). The trainer +
  gate are deliberately split into two artefacts so the gate is
  reviewable/auditable separately from the trainer.
- **Neutral / follow-up**: the CI workflow wiring
  (`.github/workflows/tests-and-quality-gates.yml` — adding a job that
  invokes `scripts/ci/ensemble_prod_gate.py`) is **out of scope** here
  because there are no real `loso_seed{N}.json` artefacts to gate yet.
  A follow-up PR — the actual flip PR — wires the workflow once a
  real-corpus run produces the JSON.
- **Neutral / follow-up**: conformal calibration on top of the
  ensemble (per ADR-0279) remains in the probabilistic-head backlog;
  this ADR addresses the **flip mechanism** for the deterministic
  ensemble members, not the calibrated-interval surface.

## References

- req (2026-05-05, user direction): the user requested a follow-up to
  PR #372 that ships the LOSO trainer + CI gate scaffold so the
  ensemble seeds can flip from `smoke: true` to `smoke: false` after
  a real LOSO run. Paraphrased: "land the trainer scaffold + gate
  script now; actual production flip is gated on a real-corpus LOSO
  clearing ≥0.95 mean PLCC plus ≤0.005 variance."
- [Research-0075](../research/0075-fr-regressor-v2-ensemble-prod-flip.md)
  — ensemble theory (Lakshminarayanan 2017), conformal calibration
  sketch (Romano 2019), 9-fold LOSO protocol, expected PLCC baseline.
- [ADR-0291](0291-fr-regressor-v2-prod-ship.md) — deterministic v2
  prod flip; defines the 0.95 LOSO PLCC ship gate this ADR inherits.
- [ADR-0279](0279-fr-regressor-v2-probabilistic.md) — probabilistic
  head scaffold; the parent of the ensemble surface this ADR flips.
- [ADR-0235](0235-codec-aware-fr-regressor.md) — codec-aware decision
  0.95 LOSO PLCC ship gate.
- [ADR-0237](0237-quality-aware-encode-automation.md) — `vmaf-tune`
  Phase A consumer + `--quality-confidence` flag that needs the
  ensemble's predictive distribution.
- PR #372 — ensemble scaffold (5 smoke seeds in registry).
- Lakshminarayanan, B., Pritzel, A., Blundell, C. (2017). _Simple
  and Scalable Predictive Uncertainty Estimation using Deep
  Ensembles._ NIPS 2017.
- Romano, Y., Patterson, E., Candès, E. (2019). _Conformalized
  Quantile Regression._ NeurIPS 2019.

### Status update 2026-05-08: Phase 2 training script landed

A second consumer of the ADR-0303 production-flip gate landed in
`ai/scripts/train_predictor_v2_realcorpus.py` — the per-codec
predictor v2 real-corpus LOSO trainer (Phase 2 of the predictor
pipeline; companion to PR #450 / PR #462). The trainer applies the
**same two-part gate** documented above (mean PLCC ≥ 0.95 across
LOSO folds, max-min spread ≤ 0.005) **per codec** rather than per
ensemble seed; the constants are mirrored as
`SHIP_GATE_MEAN_PLCC` / `SHIP_GATE_PLCC_SPREAD_MAX` /
`SHIP_GATE_PER_FOLD_MIN` in
`ai/scripts/train_predictor_v2_realcorpus.py` and continue to live
authoritatively in `scripts/ci/ensemble_prod_gate.py` for the
ensemble. A future ADR change that lowers either threshold MUST
update both call sites in lockstep; the test
`test_gate_constants_match_adr_0303` in
`ai/tests/test_train_predictor_v2_realcorpus.py` pins the
predictor-side values to the ADR-0303 §Decision numbers.

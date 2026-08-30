# ADR-0393: `fr_regressor_v2` probabilistic head — deep-ensemble + conformal scaffold

- **Status**: Accepted
- **Status update 2026-05-15**: scaffold implemented;
  `model/tiny/fr_regressor_v2_ensemble_v1.json` (5-seed ensemble) +
  conformal quantile scaffold present on master. Production training
  gated on Phase A corpus completion.
- **Date**: 2026-05-03
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, fr-regressor, probabilistic, ensemble, conformal, fork-local

## Context

The codec-aware [`fr_regressor_v2`](0272-fr-regressor-v2-codec-aware-scaffold.md)
emits a single MOS scalar per frame — a **point estimate**. Producers
running quality-aware encode automation
([ADR-0237](0237-quality-aware-encode-automation.md), the in-flight
`vmaf-tune` tool) need a stronger contract: _"give me a CRF such that
the **lower** bound of a 95 % VMAF interval is still ≥ 92"_. A point
estimate cannot answer that question — it takes a distribution.

PR #354's audit Bucket #18 (top-3 ranked) calls for a probabilistic
head on top of v2: a deep ensemble of small MLPs trained under
different seeds, optionally calibrated by split-conformal prediction.
The audit cited Lakshminarayanan et al. (2017) (deep ensembles
dominate single-network uncertainty estimators in calibration quality)
and Romano et al. (2019) (normalised conformal gives a marginal
coverage guarantee at no training-time cost beyond the calibration
split).

The constraints in play:

1. **No architecture change.** v2 is the in-flight scaffold (PR #347);
   we cannot fork its training stack just to add uncertainty.
2. **Inference cost stays small.** v2 members are 6→64→64→1 MLPs
   (~5 KB each); 5 of them is still a rounding error on libvmaf's
   per-frame budget vs the existing tiny-AI surfaces.
3. **The ONNX graph stays op-allowlist clean** — no
   sampling / dropout-at-inference / heteroscedastic NLL hacks that
   would add new ops the runtime loader rejects.
4. **The shipped checkpoint is a smoke probe.** No multi-codec Phase A
   corpus exists yet; the scaffold's job is to wire the data path so
   the production training run is a one-liner when the corpus lands.

## Decision

Add a deep ensemble of N=5 `fr_regressor_v2` members trained under
distinct random seeds, packaged as 5 separate ONNX files plus an
ensemble manifest (`model/tiny/fr_regressor_v2_ensemble_v1.json`) that
records the member list, feature standardisation, codec vocabulary,
nominal coverage, and an optional conformal residual quantile.
Inference aggregates the 5 outputs into `(mu, sigma)` and exposes the
prediction interval via two interchangeable rules: a Gaussian
`mu ± z(α/2) · σ` baseline and an opt-in `mu ± q · σ` form where
`q` is the empirical residual quantile from a held-out split-conformal
calibration set. The scaffold ships in smoke-only mode (synthetic
100-row corpus, 1 epoch per member); production training is gated on a
multi-codec Phase A corpus and tracked as backlog item
**T7-FR-REGRESSOR-V2-PROBABILISTIC**.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Deep ensemble (N=5) + conformal** ✅ | Best-in-class calibration on regression benchmarks (Lakshminarayanan 2017); conformal adds distribution-free coverage guarantee; reuses v2 architecture verbatim | 5× training cost; 5× inference cost (mitigated by tiny model size); manifest layer is new | Chosen — calibration quality dominates, inference cost is negligible, scaffold cost is one training script + one eval script. |
| Single-network heteroscedastic NLL | 1× cost; one ONNX graph | Captures aleatoric noise only — collapses on out-of-distribution inputs (Lakshminarayanan §4.2); empirically worse calibration than 5-member ensemble | Rejected — the audit's whole point is distribution-shift coverage (new codecs, OOD CRFs); aleatoric-only would fail silently at the boundary. |
| MC-dropout (T forward passes) | 1 ONNX file; cheap to train | Requires dropout-at-inference, which torch's ONNX export typically _folds away_; would need a custom `Dropout` op or onnxruntime extension | Rejected — would force a custom op into the libvmaf op allowlist (ADR-0039) for marginal benefit over deep ensemble. |
| Quantile regression (multi-output) | 1 forward pass yields the interval directly | Trains 3 separate quantile heads against pinball loss; tighter intervals only when the noise model matches; no built-in coverage guarantee without conformal | Rejected — strictly worse than deep-ensemble + conformal; no ablation budget to explore both. |
| Bayesian last-layer (Laplace, SWAG) | Theoretically grounded posterior | Requires a Hessian / second-moment pass; more ONNX-export friction; no library precedent in `vmaf_train` | Rejected — engineering surface area not justified for the marginal calibration gain over deep ensembles. |
| Single-network + bootstrap on data | Same architecture as v1 | Bootstrap captures _data_ uncertainty only (not model uncertainty); needs N retrainings _plus_ resampling | Rejected — strictly dominated by deep-ensemble (no resample bookkeeping, captures both noise sources). |

## Consequences

- **Positive**:
  - Producers can drive `vmaf-tune --quality-confidence 0.95 --target
    92` off a published, calibrated coverage guarantee instead of a
    point estimate plus folklore margin.
  - The scaffold is _additive_: existing v2 deterministic consumers
    keep working untouched; the ensemble manifest lives next to the
    per-member ONNX files in `model/tiny/`.
  - Conformal calibration is _opt-in_ — when the calibration split
    falls below a usable threshold (small corpus, OOD test) the
    manifest silently falls back to the Gaussian rule. No silent
    fail.

- **Negative**:
  - 5× ONNX inference cost at the libvmaf C-side adapter layer
    (T7-FR-REGRESSOR-V2-PROBABILISTIC follow-up). Mitigated by the
    tiny model size (~3 KB graph per member) — even serial CPU
    evaluation of 5 members is well under one decoded frame's
    per-pixel cost.
  - 5× registry entries per ensemble (one per member). Tolerated to
    avoid a registry-schema bump; the manifest sidecar is the
    ensemble-level entry point.
  - The 1.96 Gaussian assumption is calibrated _only_ on Gaussian
    residuals — without conformal, real-world coverage will
    deviate. Captured in the model card and the eval script's
    coverage report.

- **Neutral / follow-ups**:
  - T7-FR-REGRESSOR-V2-PROBABILISTIC: production training run on the
    Phase A multi-codec corpus once it lands; gated on clearing the
    v2 deterministic ship floor and on the eval script's empirical
    95 % coverage being within 5 pp of nominal.
  - C-side runtime adapter (read manifest, open 5 sessions, fan-out
    inputs, aggregate `mu / sigma`) — separate PR after this
    scaffold; exposes `vmaf_dnn_score_with_interval` to
    `core/src/dnn/`.
  - `vmaf-tune --quality-confidence` flag — Phase B follow-up to
    ADR-0237; consumer of the new C-side adapter.

## References

- Lakshminarayanan, Pritzel, Blundell (2017),
  [_Simple and Scalable Predictive Uncertainty Estimation using Deep Ensembles_](https://arxiv.org/abs/1612.01474),
  NeurIPS.
- Vovk, Gammerman, Shafer (2005), _Algorithmic Learning in a Random
  World_, Springer.
- Romano, Patterson, Candès (2019),
  [_Conformalized Quantile Regression_](https://arxiv.org/abs/1905.03222),
  NeurIPS.
- Lei, G'Sell, Rinaldo, Tibshirani, Wasserman (2018), _Distribution-Free
  Predictive Inference for Regression_, JASA.
- [Research-0054](../research/0067-fr-regressor-v2-probabilistic.md) —
  audit digest backing this ADR (PR #354 Bucket #18 ranking + literature
  pull).
- [ADR-0272](0272-fr-regressor-v2-codec-aware-scaffold.md) — parent v2
  scaffold (placeholder ID; PR #347 may land it at ADR-0261; renumber
  the cross-reference at merge time if it does).
- [ADR-0237](0237-quality-aware-encode-automation.md) — vmaf-tune Phase
  A; downstream consumer of the probabilistic interval API.
- [ADR-0039](0039-onnx-runtime-op-walk-registry.md) — runtime
  op-allowlist constraint that ruled out MC-dropout.
- [ADR-0040](0040-dnn-session-multi-input-api.md),
  [ADR-0041](0041-lpips-sq-extractor.md) — multi-input ONNX precedent
  the v2 ensemble member graph follows.
- Source: `req` (PR #354 audit Bucket #18, top-3 ranked).

### Status update 2026-05-08: implementation landed

Per ADR-0028 immutability, the body above is frozen at proposal time.
The `Status` line at the head of this ADR remains `Proposed` until
the production training run lands; this section records the
implementation deliverables that ship today as a non-binding addendum.

**What landed:** the conformal-prediction surface itself. New
`tools/vmaf-tune/src/vmaftune/conformal.py` ships
`SplitConformalCalibration` (Lei et al. 2018 Theorem 2.2) and
`CVPlusConformalCalibration` (Barber et al. 2021 Theorem 1) as a
pure-Python, dependency-free wrapper around the existing
`Predictor` surface. The CLI gains `vmaf-tune predict
--with-uncertainty --calibration-sidecar <path> [--alpha <a>]` per
the Decision section above; without a sidecar the wrapper degrades
to `low == high == point` and the report is flagged
`uncalibrated` so consumers don't silently treat a width-zero
interval as a real coverage guarantee. Empirical coverage on the
synthetic Gaussian-noise corpus matches the nominal `1 - alpha`
within ~0.01 (0.9515 vs 0.95 nominal on a 2000-point probe with a
400-point calibration set), confirming the marginal-coverage proof
in operation.

**What remains gated:** the deep-ensemble member training run, the
C-side runtime adapter (`vmaf_dnn_score_with_interval`), and the
`vmaf-tune --quality-confidence` Phase B consumer. Those land
under `T7-FR-REGRESSOR-V2-PROBABILISTIC` once the multi-codec
Phase A corpus is available; flipping `Status` to `Accepted` is
gated on that PR.

### Status update 2026-05-09: recommend + ladder consume uncertainty

Per ADR-0028 immutability the body above stays frozen; this entry
records the second downstream consumer of the conformal surface
without changing the original decision.

**What landed:** `tools/vmaf-tune/src/vmaftune/uncertainty.py`
centralises the `ConfidenceThresholds` dataclass, the
`load_confidence_thresholds` sidecar loader, and the
`classify_interval` width-band helper. The defaults
`tight_interval_max_width=2.0` and `wide_interval_min_width=5.0`
VMAF mirror the documented floor in the auto-driver F.3 work
(PR #495 / Research-0067) byte-for-byte, so a single calibration
sidecar drives `auto`, `recommend`, and `ladder` without
divergence.

`recommend.py` gains
`pick_target_vmaf_with_uncertainty(rows, UncertaintyAwareRequest)`
plus a `--with-uncertainty` / `--uncertainty-sidecar` CLI flag
pair on the `recommend` subcommand. Tight intervals short-circuit
the search at the first row whose conformal lower bound clears
the target (O(k) instead of O(n)); wide intervals force a full
scan with the result tagged `(UNCERTAIN)`; middle-band and
uncalibrated rows defer to the existing point-estimate predicate
verbatim. An `interval_excludes_target` helper surfaces a
best-effort UNMET row when every visited interval lies below the
target.

`ladder.py` gains `UncertaintyLadderPoint`,
`prune_redundant_rungs_by_uncertainty` (drops adjacent rungs
whose intervals overlap above `DEFAULT_RUNG_OVERLAP_THRESHOLD =
0.5`), `insert_extra_rungs_in_high_uncertainty_regions` (inserts
geometric-bitrate / arithmetic-VMAF mid-rungs into wide-interval
gaps), and the composed `apply_uncertainty_recipe` entry point.
The existing convex-hull + knee-selection invariants are
preserved.

**Threshold provenance:** every numeric default in this PR cites
either Research-0067 (Phase F feasibility, the same emergency
floor PR #495 documents) or the parent ADR-0279 conformal proof.
No threshold was invented locally — see the
`feedback_no_guessing` rule in `CLAUDE.md`.

**Out of scope (deferred to follow-up):** wiring the production
sampler in `_default_sampler` to emit `UncertaintyLadderPoint`
directly when the predictor ships a calibration sidecar. The
library API is fully functional today and exercised by the unit
tests (`test_recommend_uncertainty.py`,
`test_ladder_uncertainty.py`); the CLI `ladder
--with-uncertainty` flag emits an informational notice noting the
follow-up.

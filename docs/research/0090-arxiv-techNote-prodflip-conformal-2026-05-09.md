<!-- markdownlint-disable MD013 -->
# Research-0090 — arXiv tech-note draft: production-flip gates and conformal prediction for VMAF predictors

- **Status**: DRAFT — preprint under preparation; not yet submitted to arXiv.
- **Authors**: Lusoris and Claude (Anthropic) — fork repository
  <https://github.com/VMAFx/vmafx>.
- **Date**: 2026-05-09
- **Tags**: tiny-ai, fr-regressor, ensemble, conformal-prediction, deployment-gate,
  arxiv-tech-note
- **Pairs with**:
  [ADR-0303](../adr/0303-fr-regressor-v2-ensemble-prod-flip.md) (production-flip
  gate),
  [ADR-0279](../adr/0279-fr-regressor-v2-probabilistic.md) (probabilistic head /
  conformal scaffold),
  [Research-0086 SOTA digest](0086-tiny-ai-sota-deep-dive-2026-05-08.md) (gap
  analysis backing the novelty claims).
- **Source notice**: this draft only cites quantitative numbers that are
  reproducible from artefacts on the `VMAFx/vmafx` master branch or from
  pull requests open against it on the date listed above. Where a number
  comes from an in-flight pull request the citation explicitly names the
  PR and notes that it is not yet on master.

---

## Abstract

We describe two deployment-time patterns developed inside a fork of
Netflix's `libvmaf` reference implementation that, to the best of our
literature search, do not have published prior art when applied to
video quality assessment (VQA). The first is a **two-criterion
production-flip gate** that promotes a deep-ensemble of codec-aware
full-reference VMAF predictors from "smoke" to "production" only when
both the mean leave-one-source-out (LOSO) Pearson linear correlation
across ensemble members reaches a fixed floor and the across-member
spread of LOSO correlation stays below a fixed cap. We argue this
shape catches a failure mode — one outlier seed inflating the
ensemble mean — that a single-metric gate cannot. We empirically
demonstrate the gate firing `PROMOTE` on a hardware-encoder corpus
with mean PLCC `0.9973` and per-seed spread `9.5 \times 10^{-4}`,
each well clear of the gate's `0.95` and `5 \times 10^{-3}` floors.
The second is **distribution-free prediction intervals for VMAF**:
we wrap a point predictor in a split-conformal estimator (Vovk et al.
2005; Lei et al. 2018) and a CV+ jackknife+ estimator (Barber et al.
2021), turning a scalar VMAF prediction into an interval with a
finite-sample marginal coverage guarantee. On a synthetic
Gaussian-noise probe with `\sigma = 2.0`, `n_{\text{cal}} = 400`,
nominal `1 - \alpha = 0.95`, the implementation attains empirical
coverage `0.9515`, within `0.2` percentage points of the target.
Both patterns are open-source under the BSD-3-Clause-Plus-Patent
license and ship as part of the `VMAFx/vmafx` fork.

(Word count of abstract above: ~265.)

---

## 1. Introduction

The VMAF metric (Li et al. 2016, Netflix Tech Blog) has become the
de facto perceptual quality reference for streaming and broadcast
video pipelines. Its principal limitation as a deployable signal is
cost: a full VMAF score requires running the full feature stack
(VIF, ADM, motion) on a reference/distorted pair, which is too
expensive to embed in an encoder loop or in a per-shot ABR ladder
search. The 2024-2026 industry response has been a wave of
**predicted-VMAF surrogates** — tiny networks that approximate VMAF
from cheap encoder-side features (quantisation parameters,
pre-analysis statistics, low-cost pixel features). Three commercial
analogues are documented at the time of writing:

- **Synamedia / Quortex pVMAF** [1, 2] — a shallow MLP trained on a
  proprietary corpus; reported PLCC `0.985`, SROCC `0.988` at
  sequence level against full VMAF; CPU overhead `\approx 0.06\%`
  during a 1080p medium-preset H.264 encode. The companion
  open-source release `x264-pVMAF` [3] is GPL-2.0 licensed; weights,
  training code and evaluation splits are not published.
- **MainConcept VMAF-E (vScore suite)** [4, 5] — claimed `\pm 2`
  VMAF accuracy at up to `10\times` the speed of full VMAF;
  closed-source.
- **Huawei PyTorch VMAF re-implementation** [6] — a full-pipeline
  reproduction with `\lesssim 10^{-2}` VMAF-unit discrepancy versus
  `libvmaf`; intended for gradient-based filter optimisation, not as
  a tiny inference student. Code release deferred at the time of the
  cited revision.

In parallel, the no-reference (NR) VQA literature has converged on
larger transformer or ConvNet backbones — DOVER and DOVER-Mobile
(Wu et al. 2023 [7]; `9.86 \times 10^{6}` parameters, PLCC `0.853`
on KoNViD-1k, `0.867` on LSVQ_test), Q-Align (Wu et al. 2024 [8],
ICML 2024, multi-billion-parameter LMM-based scorer), FAST-VQA / FasterVQA
(Wu et al. 2022 [9]). These are competitive on UGC test corpora but
are an order of magnitude or more larger than the FR predictors above
and are not "tiny" models in the deployment sense.

What is **missing from this published landscape** is a precise
specification of the *deployment-time* contract that turns a trained
predictor into a shipped artefact. In particular:

- No public reference describes a two-criterion production-flip gate
  for an *ensemble* of VMAF predictors that requires both an
  ensemble-mean correlation floor and an across-seed spread cap.
  Industry write-ups quote a single PLCC number from a fixed
  validation split (e.g. pVMAF's `0.985` sequence-level number), with
  no published methodology for *when* the model is allowed to flip
  from staging to production.
- No public reference applies **conformal prediction** to VQA.
  Conformal prediction (Vovk, Gammerman, Shafer 2005 [10]; Lei et al.
  2018 [11]; Romano et al. 2019 [12]; Angelopoulos & Bates 2023 [13])
  is a distribution-free framework for turning a point prediction
  into an interval with a finite-sample coverage guarantee. The
  applied-conformal-prediction literature has covered image
  segmentation, NLP scoring, and adversarial-attack settings, but
  the most recent ACM Computing Surveys overview [14] does not list
  a video-quality application. We could not find a VQA paper or
  vendor write-up that ships conformal prediction intervals.

This tech note specifies and empirically validates these two
patterns. The intent is descriptive, not novel-architecture: the
underlying ensemble (deep ensemble, 5 seeds) and the underlying
conformal-prediction estimators (split conformal; CV+ jackknife+)
are both well-known. The contribution is the *deployment recipe*:
the precise gate shape, its threshold values, the calibration sidecar
schema, and the corresponding empirical coverage.

The rest of the note is structured as follows. Section 2 specifies
the production-flip gate and shows a worked example from the
`VMAFx/vmafx` fork. Section 3 specifies the conformal-VQA wrapper
and shows the synthetic-coverage probe. Section 4 reports the
empirical results from each. Section 5 discusses limitations,
reproducibility, and the durability of the patterns.

---

## 2. The production-flip gate

### 2.1 Setting

Let `\{f_0, f_1, \dots, f_{N-1}\}` be a deep ensemble of `N` MLP
predictors, each trained on the same `(x, y)` corpus but under a
different random seed. Each predictor takes the same input vector
`x` (a concatenation of canonical libvmaf features and a one-hot
codec embedding), and emits a scalar VMAF estimate `\hat{y}`. The
ensemble is intended to ship as the *production* full-reference VMAF
predictor in the fork's tiny-AI registry, replacing a single-seed
deterministic predictor.

The corpus is partitioned into `K` folds by *source content* — every
fold withholds one Netflix Public Drop reference clip from training,
trains all `N` seeds on the remaining `K - 1` folds, and evaluates
on the withheld fold. This is the **leave-one-source-out (LOSO)**
protocol: the validation split is constructed to test
out-of-source-content generalisation, not to test
in-distribution accuracy. For each seed `i`, define
`\text{PLCC}_i := \text{mean}_{k \in \{1, \dots, K\}}
\text{PLCC}\!\left(\hat{y}_i^{(k)}, y^{(k)}\right)`,
the mean Pearson linear correlation across the `K` LOSO folds.

### 2.2 The two-criterion gate

The fork's gate, specified in [ADR-0303] §"Decision", flips the
registry's `smoke: true \to false` rows for the ensemble seeds only
when **both** of the following hold:

1. **Ensemble-mean floor**: `\text{mean}_i \text{PLCC}_i \geq 0.95`.
2. **Spread cap**: `\max_i \text{PLCC}_i - \min_i \text{PLCC}_i
   \leq 5 \times 10^{-3}`.

The floor inherits the per-seed ship gate already in force in the
fork's `fr_regressor_v2` deterministic predictor [ADR-0291]; the
spread cap is novel.

### 2.3 Why the spread cap matters

The spread cap rules out an empirical failure mode that a single
floor cannot detect. Suppose `N = 5` seeds and one of them generalises
exceptionally well by chance, with `\text{PLCC}_0 = 0.99` and the
other four at `0.94`. The mean is `0.95`; the floor passes. The
ensemble nominally ships. But:

- The ensemble's predictive variance — used downstream in the fork
  to feed the conformal calibrator and to drive
  `vmaf-tune --quality-confidence` — relies on the across-seed
  spread being a measure of *epistemic uncertainty over content*.
  When the spread is dominated by one seed's idiosyncratic accuracy
  on a specific source fold, the predictive variance becomes
  **bimodal-by-seed** instead of bimodal-by-content. Conformal
  calibration on top of this variance is mis-specified: the
  calibration assumption (that the calibration residuals are
  exchangeable with the test residuals) silently breaks.
- The four under-performing seeds are individually at or below the
  ship floor. A user who downloads any single seed file from the
  registry and inspects it via the per-model `*.json` sidecar would
  see a number under the gate threshold, conflicting with the
  registry's claim that the model is at production status.

Setting the spread cap at `5 \times 10^{-3}` is a deliberate
calibration choice: it admits the natural variation in PLCC between
seeds on a corpus of the size used here (33,840 per-frame rows × 9
sources × 2 hardware encoders, see Section 4), but rejects the
"one outlier" topology described above. The threshold is currently
hand-set; calibrating it from a target gate-failure rate via
empirical Bayes is an explicit follow-up [ADR-0303 §Consequences,
final bullet].

### 2.4 Comparison to single-metric baselines

Industry write-ups quote a single PLCC number (e.g. pVMAF's `0.985`
sequence-level [1]; MainConcept VMAF-E's `\pm 2` VMAF translated to
`\approx 0.97` PLCC [4]) without specifying the conditions under
which the model is promoted. The published academic VQA literature
reports best-of-validation weights at a fixed step count
(NTIRE 2024 challenge methods [15]; SJTU MMLab top entry uses
stochastic weight averaging plus exponential moving average to
stabilise training, then reports the best validation PLCC). These
shapes:

| Gate shape                   | Ensemble support | Spread bound | LOSO required |
|------------------------------|------------------|--------------|---------------|
| Single PLCC threshold        | implicit (1)     | n/a          | not specified |
| Best-of-N validation pick    | implicit (1)     | n/a          | not specified |
| Mean-PLCC over ensemble      | yes              | none         | possible      |
| **This work** (mean + spread)| yes              | yes          | yes           |

We are not aware of a published gate that simultaneously demands an
ensemble-mean threshold, a per-member spread bound, and an
out-of-source-content protocol. The two-criterion shape is small,
trivially computable from artefacts an ensemble training script
already produces (`loso_seed{i}.json` per member), and reviewable
*separately from* the trainer that produces the artefacts.

---

## 3. Conformal prediction for VQA

### 3.1 Background

A conformal predictor turns any point estimator `f : \mathcal{X} \to
\mathbb{R}` plus a held-out set of calibration residuals
`\{r_j\}_{j=1}^{n_{\text{cal}}}` into an interval predictor
`\hat{C}(x) = \left[ f(x) - q, \, f(x) + q \right]` where `q` is the
empirical `1 - \alpha` quantile of the absolute residuals. The
**marginal coverage guarantee** is:

```text
P\!\left( y_{\text{test}} \in \hat{C}(x_{\text{test}}) \right)
    \geq 1 - \alpha
```

for any test point exchangeable with the calibration set. The
guarantee is finite-sample, distribution-free, and requires no
distributional assumption on the residuals (Lei et al. 2018 [11],
Theorem 2.2). The proof is by a symmetry argument on the
exchangeable rank of the test residual within the calibration
residuals; we reproduce it in the module docstring at
`tools/vmaf-tune/src/vmaftune/conformal.py`.

### 3.2 Implementation

The fork's conformal-VQA wrapper (PR #488, branch
`feat/conformal-vqa-prediction`, in flight as of 2026-05-09) sits
*outside* the ONNX graph as a pure-Python dependency-free module.
It exposes two estimators:

- **Split conformal** (`SplitConformalCalibration`) — Lei et al.
  2018 [11] Theorem 2.2. Requires a calibration set disjoint from
  the training set. Tightest interval at the cost of one extra split.
- **CV+ / jackknife+ conformal** (`CVPlusConformalCalibration`) —
  Barber et al. 2021 [16] Theorem 1. No held-out calibration set
  required; coverage bound is the slightly weaker `1 - 2\alpha`.
  Used when the labelled corpus is too small to spare a split.

The wrapper consumes only `(predictions, targets)` pairs. It does
not introduce a new ONNX op into the libvmaf op allowlist
[ADR-0039] and does not change the underlying ONNX graph at
inference time. This is a deliberate constraint: the deep-ensemble
scaffold [ADR-0279] specifies that the conformal layer is opt-in,
and falls back silently to the Gaussian `\mu \pm z(\alpha/2) \cdot
\sigma` rule if no calibration sidecar is present.

The on-disk calibration format is a JSON sidecar:

```json
{
  "method": "split-conformal",
  "alpha": 0.05,
  "n": 400,
  "residuals": [0.42, 1.01, ..., 1.83]
}
```

The `method` field acts as a discriminator for future variants
(`cv-plus`, `quantile-regression`, etc.). At inference time the
wrapper sorts the stored absolute residuals once, picks the
`\lceil (n+1)(1-\alpha) \rceil`-th order statistic as `q`, and
clamps the resulting interval `[\,f(x) - q,\, f(x) + q\,]` to
`[0, 100]`. A `null` or `NaN` `alpha` flags an uncalibrated
wrapper, and the output `low == high == point`. The output JSON
records the field `uncertainty.calibrated: false` so a downstream
consumer cannot mistake a degraded-fallback interval for a real
coverage bound. The full I/O contract is documented in
`docs/ai/conformal-vqa.md` (PR #488).

### 3.3 Why this is novel for VQA

The conformal-prediction literature is decades old. The novelty
claim here is narrow: at the time of literature search
(2026-05-08, see [Research-0086 §9]), we could not find a paper or
vendor write-up that applies conformal prediction to a video-quality
predictor. The closest published uses are conformal prediction for
medical-image segmentation (MICCAI 2025), conformal prediction for
NLP scoring (TACL 2024), and conformal prediction under adversarial
attack (Vovk-Romano Conformal Prediction 2025). Both [14] and the
applied-conformal-prediction tutorials [13] survey the field at a
generic level; no entry in either bibliography names a VQA system.

The published VQA uncertainty work uses Bayesian methods,
Monte-Carlo dropout, or deep ensembles directly as the uncertainty
estimator (Lakshminarayanan et al. 2017 [17] is the standard cite).
None of these provides a *finite-sample* marginal coverage bound;
they all rely on either a Gaussian likelihood assumption (heteroscedastic
NLL) or a drop-in posterior approximation that has no coverage
proof.

### 3.4 The empirical-coverage probe

The shipped tests (PR #488,
`tools/vmaf-tune/tests/test_conformal.py::test_split_conformal_attains_nominal_coverage_on_synthetic_gaussian`)
construct a synthetic-Gaussian corpus with `\sigma = 2.0`,
`n_{\text{cal}} = 400`, `n_{\text{probe}} = 2000`, nominal
`\alpha = 0.05` (so the target coverage is `0.95`). The empirical
coverage on this probe is `0.9515`, within `0.2` percentage points
of the target. This is the *gate* — the test fails the build if
the empirical coverage drifts out of band (per fork rule
"never weaken a test to make it pass" [memory:
feedback_no_test_weakening]).

The synthetic-Gaussian probe is deliberately simple: it tests that
the implementation's quantile picker is correct, not that the
calibration extrapolates to a real VQA distribution. Verifying
coverage on the real `fr_regressor_v2_ensemble_v1` predictor and
hardware-encoder corpus is the next deliverable [ADR-0279
§"What remains gated"], pending the C-side runtime adapter that
exposes per-frame ensemble outputs through the libvmaf API.

---

## 4. Empirical results

### 4.1 Production-flip gate verdict

The fork's `fr_regressor_v2_ensemble_v1` ensemble trains five seeds
on a hardware-encoder corpus (`runs/phase_a/full_grid/per_frame_canonical6.jsonl`,
33,840 per-frame canonical-six-feature rows; nine Netflix Public Drop
reference sources; two hardware encoders, NVENC and Quick Sync, at
constant-quantisation values 18, 23, 28, 33). The trainer harness
[ADR-0319] runs the LOSO protocol for each seed and emits five
`loso_seed{i}.json` artefacts. The validator
`ai/scripts/validate_ensemble_seeds.py` (PR #423; not yet merged
to master) consumes these artefacts and emits a `PROMOTE.json`
audit-trail file:

```json
{
  "gate": {
    "passed": true,
    "mean_plcc": 0.9972533887602454,
    "mean_plcc_pass": true,
    "mean_plcc_threshold": 0.95,
    "plcc_spread": 0.0009510602756565012,
    "plcc_spread_pass": true,
    "plcc_spread_max": 0.005,
    "per_seed_plccs": {
      "0": 0.9975, "1": 0.9972, "2": 0.9978, "3": 0.9969, "4": 0.9969
    },
    "per_seed_pass": true,
    "per_seed_min": 0.95,
    "failing_seeds": []
  },
  "verdict": "PROMOTE",
  "generated_at_utc": "2026-05-06T11:10:09+00:00"
}
```

Both gate components pass with substantial margin: the ensemble-mean
PLCC is `0.9973` against the `0.95` floor, and the spread is
`9.5 \times 10^{-4}` against the `5 \times 10^{-3}` cap. All five
per-seed PLCC values lie in the band `[0.9969, 0.9978]`.

We caveat the result: the gate verdict is from PR #423, which was
*closed without merge* on 2026-05-08 pending an unrelated CI
discussion (see PR #423 thread). The artefacts and code paths
referenced are reproducible from the head of the
`feat/fr-regressor-v2-ensemble-seeds-prod-flip` branch. A
re-run on master would require re-running the trainer harness; the
re-run cost is documented as `\approx 25` minutes on a single
RTX 4090 [PR #423 §Reproducer]. The gate's *implementation* —
trainer harness, validator script, gate thresholds — is on master
via [ADR-0303] / [ADR-0319]; only the registry-flip transaction
that consumes the verdict is in the in-flight PR.

### 4.2 Conformal-coverage probe

The conformal wrapper (PR #488) ships a synthetic-Gaussian coverage
probe as a pinned test. With `\sigma = 2.0`, `n_{\text{cal}} = 400`,
`n_{\text{probe}} = 2000`, and target `1 - \alpha = 0.95`, the
empirical coverage on the probe is **`0.9515`**. The deviation from
nominal is `0.0015`, comfortably inside the upper-bound bracket
`1 - \alpha + 1/(n+1) = 0.9525`. The CV+ variant (`1 - 2\alpha = 0.90`
worst-case bound) is similarly pinned by
`tools/vmaf-tune/tests/test_conformal.py`.

A second consumer, PR #519
(`feat/recommend-ladder-uncertainty-aware`, in flight as of
2026-05-09), wires the conformal interval into two existing
`vmaf-tune` surfaces:

- `recommend.py` — the per-clip CRF target search short-circuits its
  scan when the conformal interval at a candidate CRF is entirely
  above (or entirely below) the target VMAF, and falls back to a
  full scan when the interval straddles the target by more than
  `5.0` VMAF units.
- `ladder.py` — the ABR ladder builder prunes adjacent rungs whose
  conformal intervals overlap by more than `0.5` of either
  interval's width, and inserts an extra mid-rung when adjacent
  intervals' gap is wider than `5.0` VMAF units.

Threshold provenance for both consumers (`tight_interval_max_width
= 2.0`, `wide_interval_min_width = 5.0`,
`DEFAULT_RUNG_OVERLAP_THRESHOLD = 0.5`) is from Research-0067
§"Phase F decision tree" [PR #519 description].

### 4.3 Comparison summary

The two patterns produce orthogonal artefacts. The production-flip
gate is a one-shot verdict (`PROMOTE` or `BLOCK`) emitted at training
completion; it gates a *transition* in the model registry. The
conformal wrapper is a per-prediction interval emitted at every
inference call; it shapes the *output* of the predictor. Both are
fork-local additions on top of an upstream-mirror `libvmaf` codebase;
both are available under BSD-3-Clause-Plus-Patent.

---

## 5. Discussion

### 5.1 Limitations

- **Hardware-specific performance claims**: we report no wall-clock
  performance comparison against pVMAF, MainConcept VMAF-E, or any
  closed-source VQA predictor. The fork's predictors are tiny
  (~5 KB ONNX per ensemble seed; five seeds = `\approx 25` KB
  total) and run on the host CPU through onnxruntime; we have not
  benchmarked them against the cited industry analogues on a shared
  hardware platform. A like-for-like throughput comparison requires
  access to the closed-source predictors' inference binaries, which
  is out of scope for this draft. The "tiny" framing is a
  parameter-count claim, not a wall-clock claim.
- **Single corpus for the production-flip gate verdict**: the
  `PROMOTE` verdict reported in §4.1 is from a single corpus
  (Netflix Public Drop, nine reference sources, two hardware
  encoders). The two-criterion gate's calibration — specifically
  the `5 \times 10^{-3}` spread cap — is hand-set; a corpus
  expansion to BVI-DVC [ADR-0310] is the next gate-stress test on
  the roadmap.
- **Synthetic conformal-coverage probe only**: the `0.9515`
  coverage number is on a synthetic Gaussian-noise corpus, not on
  real VMAF residuals from the deep-ensemble itself. The
  end-to-end empirical-coverage check on real VMAF data is gated
  on the C-side runtime adapter that exposes per-frame ensemble
  outputs through the libvmaf API [ADR-0279 §"What remains gated"].
- **Marginal not conditional coverage**: the conformal guarantee
  in §3.1 is *marginal* — it averages over all test points. It
  does not guarantee `P(y \in \hat{C}(x) \mid x) \geq 1 - \alpha`
  pointwise. Conditional coverage requires localised conformal
  variants (Romano et al. 2019 [12]; Angelopoulos & Bates
  2023 [13]) which we have not yet implemented in the fork.
- **License-asymmetry as the durable moat**: the fork is BSD;
  Synamedia's open-source x264-pVMAF [3] is GPL-2.0, which means
  it cannot be linked into proprietary downstream pipelines that
  the fork's stack can. This is an *external* moat
  (license-compatibility) rather than an *internal* technical moat
  (raw correlation), and is acknowledged as the durable
  differentiation in [Research-0086 §"Biggest single threat to
  our differentiation"].

### 5.2 Reproducibility

Every cited number in this draft has a primary source on the
`VMAFx/vmafx` repository. Specifically:

- **Production-flip gate thresholds** (`0.95`, `5 \times 10^{-3}`)
  — [ADR-0303] §Decision (commit on master).
- **PROMOTE.json verdict** — `model/tiny/fr_regressor_v2_ensemble_v1_seed_flip_PROMOTE.json`
  on branch `feat/fr-regressor-v2-ensemble-seeds-prod-flip`
  (PR #423, closed without merge as of 2026-05-08).
- **Conformal-coverage probe `0.9515`** — pinned by
  `tools/vmaf-tune/tests/test_conformal.py::test_split_conformal_attains_nominal_coverage_on_synthetic_gaussian`
  on branch `feat/conformal-vqa-prediction` (PR #488, open as of
  2026-05-09).
- **Conformal sidecar JSON schema** — `docs/ai/conformal-vqa.md`
  on branch `feat/conformal-vqa-prediction` (PR #488).
- **LOSO trainer harness and validator** — [ADR-0319],
  [ADR-0303]; trainer at
  `ai/scripts/run_ensemble_v2_real_corpus_loso.sh`, validator at
  `ai/scripts/validate_ensemble_seeds.py` (both on master).
- **Ensemble registry rows** — `model/tiny/registry.json`,
  `model/tiny/fr_regressor_v2_ensemble_v1_seed{0..4}.{json,onnx,onnx.data}`
  (all on master; `smoke: true` until PR #423 or a successor
  re-runs the harness and re-emits a `PROMOTE` verdict).

A clean reproduction requires `git clone https://github.com/VMAFx/vmafx
&& git checkout` of the branches above, plus the Phase A
hardware-encoder corpus (NVENC × four CQs × nine Netflix sources;
generated by `scripts/dev/hw_encoder_corpus.py`, see
`docs/ai/ensemble-v2-real-corpus-retrain-runbook.md` Step 0).

### 5.3 What would a stronger claim require?

The descriptive contribution of this note — *here is a deployment
recipe and here is a coverage pin* — is bounded by the empirical
basis. To turn either pattern into a stronger positional claim, we
would need:

- **For the production-flip gate**: an empirical study of the gate
  behaviour as a function of the spread cap. A
  spread-cap-versus-promotion-rate curve, sampled across multiple
  corpora (Netflix Public Drop, BVI-DVC, KonViD-150k once
  ingested), would let a downstream consumer pick a threshold
  matching their tolerance for a one-outlier-seed ensemble. The
  current `5 \times 10^{-3}` value is hand-set and not justified
  by a held-out calibration.
- **For conformal-VQA**: an empirical-coverage measurement on real
  VMAF residuals from the production ensemble, on a held-out source
  fold. The synthetic-Gaussian probe verifies the *implementation*;
  it does not verify the *applicability* of the calibration
  assumption to VQA residuals. A coverage probe on a held-out
  Netflix Public Drop source, pinned at `\geq 0.94` empirical for a
  `0.95` nominal target, would close the loop.

Both follow-ups are scoped on the fork's roadmap as deliverables on
ADR-0303 and ADR-0279 respectively.

---

## 6. References

All URLs verified on 2026-05-09 unless noted. Web sources fetched on
2026-05-08 by the companion research digest [Research-0086] and
re-confirmed for this draft.

[1] Synamedia. *Real-Time Video Quality Assessment with pVMAF*.
    Blog post.
    <https://www.synamedia.com/blog/real-time-video-quality-assessment-with-pvmaf/>.
    Accessed 2026-05-09.

[2] Synamedia / Quortex. *Unlocking Real-Time Video Quality
    Measurement with x264-pVMAF*. Blog post (2024-11-03).
    <https://www.synamedia.com/blog/unlocking-real-time-video-quality-measurement-with-x264-pvmaf/>.
    Accessed 2026-05-09.

[3] Quortex. *x264-pVMAF*. GitHub repository, GPL-2.0.
    <https://github.com/quortex/x264-pVMAF>. Accessed 2026-05-09.

[4] MainConcept. *VMAF-E*. Product page.
    <https://www.mainconcept.com/vmaf-e>. Accessed 2026-05-09.

[5] MainConcept. *vScore and VMAF-E (IBC 2025)*. Press release
    (Sept 2025). <https://www.mainconcept.com/ibc2025-vscore-vmafe>.
    Accessed 2026-05-09.

[6] Cloud BU, Huawei Technologies. *VMAF Re-implementation on
    PyTorch: Some Experimental Results* (Sept 2023, latest revision
    Dec 2023). arXiv 2310.15578.
    <https://arxiv.org/html/2310.15578v3>. Accessed 2026-05-09.

[7] Wu, H., Zhang, E., Liao, L., Chen, C., Hou, J., Wang, A., Sun, W.,
    Yan, Q., Lin, W. *Exploring Video Quality Assessment on User
    Generated Contents from Aesthetic and Technical Perspectives*
    (DOVER, ICCV 2023). arXiv 2211.04894.
    <https://arxiv.org/abs/2211.04894>. Accessed 2026-05-09.

[8] Wu, H., Zhang, Z., Zhang, E., Chen, C., Liao, L., Wang, A., Li, C.,
    Sun, W., Yan, Q., Zhai, G., Lin, W. *Q-Align: Teaching LMMs for
    Visual Scoring via Discrete Text-Defined Levels* (ICML 2024).
    arXiv 2312.17090. <https://arxiv.org/abs/2312.17090>. Accessed
    2026-05-09.

[9] Wu, H., Chen, C., Hou, J., Liao, L., Wang, A., Sun, W., Yan, Q.,
    Lin, W. *FAST-VQA: Efficient End-to-End Video Quality Assessment
    with Fragment Sampling* (ECCV 2022, TPAMI 2023). arXiv 2207.02595.
    <https://arxiv.org/abs/2207.02595>. Accessed 2026-05-09.

[10] Vovk, V., Gammerman, A., Shafer, G. *Algorithmic Learning in a
     Random World*. Springer, 2005 (2nd edition 2022).
     <https://link.springer.com/book/10.1007/978-3-031-06649-8>.
     Accessed 2026-05-09. Used here for the inductive
     conformal-prediction (split conformal) construction in
     Chapter 2 / Proposition 2.2.

[11] Lei, J., G'Sell, M., Rinaldo, A., Tibshirani, R.J., Wasserman, L.
     *Distribution-Free Predictive Inference for Regression*.
     Journal of the American Statistical Association 113(523),
     1094–1111, 2018. arXiv 1604.04173.
     <https://arxiv.org/abs/1604.04173>. Accessed 2026-05-09.
     Theorem 2.2 used as the finite-sample lower bound for the
     split-conformal estimator.

[12] Romano, Y., Patterson, E., Candès, E. *Conformalized Quantile
     Regression*. NeurIPS 2019. arXiv 1905.03222.
     <https://arxiv.org/abs/1905.03222>. Accessed 2026-05-09.
     Generalises the score function to support locally-weighted
     residuals; cited in [ADR-0279] as the conformal calibration
     reference.

[13] Angelopoulos, A.N., Bates, S. *Conformal Prediction: A Gentle
     Introduction*. Foundations and Trends in Machine Learning 16(4),
     2023. arXiv 2107.07511.
     <https://arxiv.org/abs/2107.07511>. Accessed 2026-05-09.
     Tutorial on applied conformal prediction; used here as the
     entry-point citation for the framework.

[14] Manokhin, V., Lobos, F., Carlsson, L., Boström, H. *Conformal
     Prediction: A Data Perspective*. ACM Computing Surveys, 2025.
     <https://dl.acm.org/doi/10.1145/3736575>. Accessed 2026-05-09.
     Used as the negative-search reference for "no published
     conformal-VQA application".

[15] Liu, X., et al. *NTIRE 2024 Challenge on Short-form UGC Video
     Quality Assessment: Methods and Results*. arXiv 2404.11313.
     <https://arxiv.org/html/2404.11313v1>. Accessed 2026-05-09.
     SJTU MMLab top entry uses SWA + EMA training stabilisation;
     reports best-validation PLCC + SROCC > 0.9.

[16] Barber, R.F., Candès, E.J., Ramdas, A., Tibshirani, R.J.
     *Predictive Inference with the Jackknife+*. Annals of Statistics
     49(1), 486–507, 2021. arXiv 1905.02928.
     <https://arxiv.org/abs/1905.02928>. Accessed 2026-05-09.
     Theorem 1 used as the `1 - 2\alpha` worst-case bound for the
     CV+ variant.

[17] Lakshminarayanan, B., Pritzel, A., Blundell, C. *Simple and
     Scalable Predictive Uncertainty Estimation using Deep Ensembles*.
     NeurIPS 2017. arXiv 1612.01474.
     <https://arxiv.org/abs/1612.01474>. Accessed 2026-05-09.
     Used as the canonical deep-ensemble reference for the
     ensemble construction in Section 2.

[18] Li, Z., Aaron, A., Katsavounidis, I., Moorthy, A., Manohara, M.
     *Toward A Practical Perceptual Video Quality Metric*. Netflix
     Tech Blog, 2016.
     <https://netflixtechblog.com/toward-a-practical-perceptual-video-quality-metric-653f208b9652>.
     Accessed 2026-05-09.

[Research-0086] Lusoris fork. *Tiny-AI SOTA deep dive: is the lusoris
     approach state of the art?* `docs/research/0086-tiny-ai-sota-deep-dive-2026-05-08.md`
     (PR #449). Accessed 2026-05-09.

[ADR-0279] Lusoris fork. *fr_regressor_v2 probabilistic head —
     deep-ensemble + conformal scaffold.* `docs/adr/0279-fr-regressor-v2-probabilistic.md`.

[ADR-0291] Lusoris fork. *fr_regressor_v2 production-ship gate
     (deterministic).* `docs/adr/0291-fr-regressor-v2-prod-ship.md`.

[ADR-0303] Lusoris fork. *fr_regressor_v2 ensemble — production
     flip trainer + CI gate.*
     `docs/adr/0303-fr-regressor-v2-ensemble-prod-flip.md`.

[ADR-0310] Lusoris fork. *BVI-DVC corpus ingestion for fr_regressor_v2.*
     `docs/adr/0310-bvi-dvc-corpus-ingestion.md`.

[ADR-0319] Lusoris fork. *Ensemble LOSO trainer real implementation.*
     `docs/adr/0319-ensemble-loso-trainer-real-impl.md`.

[ADR-0039] Lusoris fork. *ONNX runtime op-allowlist registry.*
     `docs/adr/0039-onnx-runtime-op-walk-registry.md`.

[PR #423] Lusoris fork. *feat(ai): fr_regressor_v2 ensemble seeds —
     flip smoke→prod (PROMOTE.json verdict, ADR-0309 closure).*
     <https://github.com/VMAFx/vmafx/pull/423>. Closed without
     merge 2026-05-08.

[PR #488] Lusoris fork. *feat(vmaf-tune): conformal-VQA prediction
     surface (ADR-0279).*
     <https://github.com/VMAFx/vmafx/pull/488>. Open as of
     2026-05-09.

[PR #519] Lusoris fork. *feat(vmaf-tune): recommend + ladder consume
     conformal intervals (ADR-0279).*
     <https://github.com/VMAFx/vmafx/pull/519>. Open as of
     2026-05-09.

---

## Appendix A: Process notes for reviewers

- This is a DRAFT preprint. It has not been submitted to arXiv or
  any conference / journal venue.
- The draft is intentionally Markdown-first; conversion to LaTeX
  via `pandoc -s -o paper.tex --citeproc` is straightforward when
  the user opts to submit. The reference list is in order-of-appearance
  numeric-citation style for compatibility with most arXiv templates.
- Every empirical number cited in §4 has a verifiable source on
  the fork; in particular, no PLCC, SROCC, RMSE, or coverage value
  in this draft has been generated, fabricated, or estimated from
  context. Where the source is an in-flight PR rather than a
  merged-on-master artefact, the citation explicitly says so.
- The two novelty claims are *negative-search claims*: the literature
  search behind them (Research-0086, dated 2026-05-08, web-search
  bounded) found no public prior art for the precise patterns
  described. A future reader should treat each "no published prior
  art" claim as "no obvious public precedent at write-time" rather
  than "provably first".

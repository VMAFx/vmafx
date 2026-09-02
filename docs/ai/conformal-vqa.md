<!-- markdownlint-disable MD013 -->
# Conformal VQA — distribution-free prediction intervals

The conformal-VQA wrapper turns any point predictor under
`tools/vmaf-tune/` into an interval predictor with a *distribution-free,
finite-sample* coverage guarantee. Where the existing `Predictor`
class returns "predicted VMAF = 87.3", the conformal wrapper returns
"predicted VMAF in [85.2, 89.4] with 95 % probability" — and that
95 % is a real coverage bound on exchangeable data, not an
informal confidence statement.

## What the wrapper does

`vmaftune.conformal.ConformalPredictor(base, calibration)` takes any
predictor with a `predict_vmaf(features, crf, codec) -> float` method
and a calibration object built from a held-out residual set. Each
`predict()` call returns a `(point, low, high)` triple plus the
nominal miscoverage level `alpha`.

Two calibration estimators ship:

- **Split conformal** (`SplitConformalCalibration`) — the
  ``Lei et al. 2018`` form. Wants a calibration set disjoint from
  training. Cheapest, tightest bound.
- **CV+ / jackknife+ conformal** (`CVPlusConformalCalibration`) —
  the ``Barber et al. 2021`` form. No holdout needed; coverage bound
  weakens to `1 - 2*alpha`. Use this when the labelled corpus is too
  small to spare a calibration split.

Both estimators consume only `(predictions, targets)` pairs — there
is no architecture change to the underlying ONNX models, no new
runtime dependency, and no change to the existing op allowlist.

## Output range and interpretation

- **`point`** — the underlying predictor's VMAF estimate, in
  `[0, 100]`. Identical to what `Predictor.predict_vmaf` returns.
- **`low`, `high`** — the conformal prediction interval, clamped to
  `[0, 100]`. By construction, ``low <= point <= high``. The
  marginal coverage guarantee is
  ``P(target_vmaf in [low, high]) >= 1 - alpha`` for any test point
  exchangeable with the calibration set (Lei et al. 2018
  Theorem 2.2 for split; Barber et al. 2021 Theorem 1 for CV+).
- **`alpha`** — the nominal miscoverage level. Default `0.05`
  (95 % interval). A `null` / `NaN` value signals an uncalibrated
  wrapper: `low == high == point` and the interval is *not* a
  coverage bound. Persisted calibration sidecars are strict JSON:
  non-finite diagnostic values are written as `null`, not as
  JavaScript-only `NaN` / `Infinity` tokens.

The interval **does not** cover model-specification error or
distribution shift. If the calibration set is drawn from a different
distribution than the test set, the lower bound silently fails. The
shipped `coverage_probe()` diagnostic flags the most common form of
this drift via a `MiscalibrationWarning`.

## CLI usage example

The `vmaf-tune predict` subcommand gains three new flags. Without a
calibration sidecar the wrapper degrades to a width-zero interval and
the `uncertainty.calibrated` field in the output is `false`.

```bash
# Baseline — no uncertainty (existing behaviour).
vmaf-tune predict --source ref.mkv --target-vmaf 92

# With uncertainty + a shipped split-conformal sidecar.
vmaf-tune predict \
    --source ref.mkv \
    --target-vmaf 92 \
    --with-uncertainty \
    --calibration-sidecar model/predictor_libx264_calibration.json \
    --alpha 0.05
```

Sample JSON output (one residual row, `alpha=0.05`):

```json
{
  "verdict": "GOSPEL",
  "uncertainty": {"enabled": true, "calibrated": true, "alpha": 0.05},
  "residuals": [
    {
      "shot_start": 0,
      "shot_end": 120,
      "crf": 23,
      "predicted_vmaf": 87.30,
      "measured_vmaf": 86.95,
      "residual": 0.35,
      "interval": {"low": 85.21, "high": 89.39, "alpha": 0.05}
    }
  ]
}
```

The Python API offers the same surface for callers that compose
`vmaf-tune` programmatically:

```python
from vmaftune.conformal import calibrate_split, save_split_calibration
from vmaftune.predictor import Predictor

# 1. Calibrate against a held-out (predictions, targets) sample.
cal = calibrate_split(predictions=cal_p, targets=cal_t, alpha=0.05)
save_split_calibration(cal, "predictor_libx264_calibration.json")

# 2. At inference time:
predictor = Predictor()
point, low, high = predictor.predict_vmaf_with_uncertainty(
    features, crf=23, codec="libx264", calibration=cal,
)
```

## Provenance and constraints

- **Implementation**: pure Python, `tools/vmaf-tune/src/vmaftune/conformal.py`.
  No new build-time dependencies — the module imports only `math`,
  `statistics`, `dataclasses`, and `json`. No new ONNX op is loaded;
  the wrapper sits *outside* the ONNX graph.
- **Sidecar JSON schema** (split conformal):
  `{"method": "split-conformal", "alpha": <float>, "n": <int>, "residuals": [<float>, ...]}`.
  Versioned by the `method` discriminator; future `cv-plus`
  sidecars use a different value.
- **License**: BSD-3-Clause-Plus-Patent, matching the rest of
  `vmaf-tune/`. The conformal-prediction theory is in the public
  domain (algorithmic results, no patent claims known to us).

## Known limitations

- **Bit-depth / colour space**: the wrapper inherits the underlying
  predictor's input contract; it neither widens nor restricts
  the supported pixel formats.
- **Minimum calibration size**: the marginal-coverage proof holds
  for any `n >= 1`, but at small `n` the upper-bound bracket
  ``1 - alpha + 1/(n+1)`` becomes loose. We recommend `n >= 100`
  per (codec, resolution-class) cell for a 5 % miscoverage target.
  At `n < 20` the wrapper still returns intervals but they are
  effectively the maximum-residual fallback.
- **Distribution shift**: marginal validity assumes the calibration
  set and the test point are exchangeable. Encoding a 4K HDR shot
  against a 1080p SDR calibration set breaks the assumption silently;
  the `coverage_probe()` diagnostic surfaces this when called with
  a held-out probe drawn from the operational distribution.
- **CPU-only**: the wrapper is pure Python and runs on the host
  thread; it is not a GPU path. The cost relative to the underlying
  ONNX inference is negligible (one quantile on a sorted residual
  vector per call site, amortised across an entire job).
- **Symmetric intervals only (split conformal)**: split conformal
  with the absolute-residual score produces a symmetric interval
  about the point estimate. For asymmetric noise (e.g. the residual
  distribution skews high near VMAF 100 because of the [0, 100]
  clamp), use `CVPlusConformalCalibration` or train a quantile head
  per ADR-0279's "Quantile regression" alternative.

## Theoretical background

Split conformal was introduced as **inductive conformal prediction**
in Vovk, Gammerman, Shafer (2005), *Algorithmic Learning in a Random
World*, Springer (Chapter 2 / Proposition 2.2). The regression-specific
treatment used here follows Lei, G'Sell, Rinaldo, Tibshirani,
Wasserman (2018), *Distribution-Free Predictive Inference for
Regression*, JASA 113(523), 1094-1111 (Theorem 2.2 — finite-sample
``1 - alpha`` lower bound, ``1 - alpha + 1/(n+1)`` upper bound).
Romano, Patterson, Candès (2019), *Conformalized Quantile Regression*,
NeurIPS, generalises the score function to support the locally
weighted / normalised residual variant. The CV+ form is from Barber,
Candès, Ramdas, Tibshirani (2021), *Predictive Inference with the
Jackknife+*, Annals of Statistics 49(1), 486-507 (Theorem 1 — the
``1 - 2*alpha`` worst-case bound that the implementation tests pin).

The full proof of marginal validity is reproduced in the module
docstring at `tools/vmaf-tune/src/vmaftune/conformal.py`.

## Cross-references

- [ADR-0279](../adr/0279-fr-regressor-v2-probabilistic.md) — the
  scoping ADR (deep-ensemble + conformal scaffold). The "Status
  update 2026-05-08" addendum tracks the implementation deliverables
  shipped here.
- [ADR-0042](../adr/0042-tinyai-docs-required-per-pr.md) — the
  per-PR doc-substance rule this page satisfies.
- `tools/vmaf-tune/tests/test_conformal.py` — the empirical-coverage
  pin, miscalibration-warning pin, and CV+ ``1 - 2*alpha`` pin.

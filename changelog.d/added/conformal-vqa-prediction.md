- **Conformal-VQA prediction surface for `vmaf-tune` predictor
  ([ADR-0279](../docs/adr/0279-fr-regressor-v2-probabilistic.md),
  [docs/ai/conformal-vqa.md](../docs/ai/conformal-vqa.md)).** New
  `tools/vmaf-tune/src/vmaftune/conformal.py` ships
  `SplitConformalCalibration` (Lei et al. 2018 Theorem 2.2) and
  `CVPlusConformalCalibration` (Barber et al. 2021 Theorem 1) as a
  pure-Python wrapper that turns any `Predictor` point estimate into
  a distribution-free prediction interval `(point, low, high)` with
  a finite-sample marginal coverage guarantee. The
  `vmaf-tune predict` subcommand gains
  `--with-uncertainty --calibration-sidecar <path> [--alpha <a>]`;
  output JSON gains an `uncertainty` block plus a per-residual
  `interval: {low, high, alpha}` field. Without a sidecar the
  wrapper degrades to `low == high == point` and the report is
  flagged `uncalibrated` so consumers don't mistake a width-zero
  interval for a real coverage bound. Empirical coverage on a
  synthetic Gaussian-noise corpus matches the nominal 95 % within
  0.2 pp (0.9515 vs 0.95) on a 2000-point probe with a 400-point
  calibration set. Pure-Python (no `numpy` / `onnxruntime`
  dependency); the conformal layer sits *outside* the ONNX graph
  so the op allowlist (ADR-0039) is unaffected.

- **`vmaf-tune fast` production wiring (ADR-0304, builds on ADR-0276
  scaffold).** The `vmaf-tune fast` subcommand graduates from
  scaffold-only to production-wired: Optuna TPE search drives a
  proxy-backed CRF→VMAF predictor (the production
  `fr_regressor_v2` ONNX shipped in ADR-0291 — no smoke models),
  followed by a single GPU-verify pass at the recommended CRF using
  the score backend selected via
  `vmaftune.score_backend.select_backend`. The verify score is
  authoritative; the proxy score is reported as a diagnostic.
  Recommendation results gain `verify_vmaf` and `proxy_verify_gap`
  fields; when the gap exceeds `--proxy-tolerance` (default 1.5
  VMAF) the result is flagged OOD so the operator knows to fall back
  to the slow Phase A grid (ADR-0276 fallback contract). Default
  trial budget is `PROD_N_TRIALS = 30` (Research-0076 §1: TPE
  converges in 30–50 trials on a single integer CRF axis); smoke
  mode keeps `SMOKE_N_TRIALS = 50` and continues to work without
  Optuna / onnxruntime / a GPU. The new `vmaftune.proxy.run_proxy`
  helper centralises ONNX loading + 14-D codec-block encoding
  (12-way ENCODER_VOCAB v2 one-hot + preset_norm + crf_norm) so
  future probabilistic-head / ensemble migrations land in one
  place.

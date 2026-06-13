- **`fr_regressor_v2_ensemble_v1_seed{0..4}` registry rows: restored
  dropped `license` / `license_url` / `sigstore_bundle` metadata and
  made the smoke/production state honest.** PR #865 regenerated the five
  ensemble ONNX files in smoke mode to fix a `codec_vocab` 14→6 input-dim
  mismatch, but also dropped the per-row license metadata and left the
  rows labelled neither production nor consistently smoke. The license
  fields are restored, `smoke: true` now truthfully describes the shipped
  weights, and the production flip is deferred to the one-shot post-RC
  retrain (ADR-1105) with a strict-xfail tracking marker on
  `test_fr_regressor_v2_ensemble_seed_rows_are_production`.

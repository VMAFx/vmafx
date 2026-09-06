- Fixed Python `vmaf-tune fast` probe extraction and normalisation defects (`T-VMAFTUNE-FAST-PY-PROBE-BROKEN-2026-08-30`):
  1. Probe `.mp4` container outputs are now decoded to temporary raw YUV via `maybe_decode_distorted` before libvmaf execution (with guaranteed cleanup), avoiding libvmaf rawvideo demuxing parser crashes.
  2. Feature extraction parses libvmaf pooled metric keys (`integer_adm2`, `integer_vif_scale0..3`, `integer_motion2`) with fallback to bare metric keys and per-frame averages.
  3. Raw features are standardised via `(x - mean) / std` using `feature_mean` and `feature_std` loaded from `model/tiny/fr_regressor_v2.json`, matching the Go twin `pkg/fast` contract.
  4. Non-zero exit statuses or failures during probe encoding and scoring raise `RuntimeError` immediately rather than silently injecting zeroes (`[0.0] * 6`).
  5. `ENCODER_VOCAB_V2` ordering aligned and unrecognized encoders map to slot 11 (`unknown`) when `allow_unknown=True`.

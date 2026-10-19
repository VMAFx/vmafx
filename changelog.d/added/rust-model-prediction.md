- **The prediction step of the `vmaf_v1.0.16*` models can run in Rust.** A build
  with `-Denable_rust_features=true` and `VMAF_FEATURE_IMPL=rust` evaluates the
  feature normalisation, chroma correction, nu-SVR, score transform and clip in
  the new `vmafx-predict` crate. The score is bit-identical to the C predictor
  at `--precision max`; the C predictor stays the default. See
  [Models](docs/models/overview.md#rust-prediction-experimental).

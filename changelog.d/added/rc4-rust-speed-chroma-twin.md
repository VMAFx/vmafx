- **`speed_chroma` has a Rust implementation that returns the C extractor's
  scores bit for bit.** Build with `-Denable_rust_features=true`, then select it
  with `--feature speed_chroma_rust` or `VMAF_FEATURE_IMPL=rust`; C stays the
  default. It covers every option the `vmaf_v1.0.16*` models set (prescale 1.0,
  0.5 and 0.6) and all four prescale methods. See
  [the SpEED page](docs/metrics/speed.md#rust-twin) and
  [ADR-1713](docs/adr/1713-rc4-rust-extractor-framework.md).

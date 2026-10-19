- **The integer `motion` extractor has a Rust twin, `motion_rust`, that returns the C
  extractor's scores bit for bit.**
  With `-Denable_rust_features=true`, `VMAF_FEATURE_IMPL=rust` (or
  `--feature motion_rust`) computes `motion_sad_score`, `motion2` and `motion3`,
  including the five-frame window and the moving average, in Rust; the default
  stays the C extractor. See [Motion](docs/metrics/motion.md#rust-implementation).

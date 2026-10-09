- **`motion_rust` writes `motion2` / `motion3` frame by frame, as the C `motion`
  does (ADR-2090).** With the Rust twin, a frame's `motion2` and `motion3`
  arrive as soon as the frame after it is scored instead of at the flush, so a
  window over a VMAF model completes during the stream with
  `VMAF_FEATURE_IMPL=rust` too; the values are unchanged, bit for bit. A Rust
  twin now gets its own `advance` callback where its C extractor has one,
  instead of inheriting the C callback, which made `motion_rust` fail at the
  flush. See [Motion](docs/metrics/motion.md#rust-implementation).

- **Rust twins of C feature extractors, selectable at run time (RC4
  framework, [ADR-1713](docs/adr/1713-rc4-rust-extractor-framework.md)).**
  A build with `-Denable_rust_features=true` links one Rust archive into
  `libvmaf` and registers each Rust twin as `<name>_rust` next to its C
  extractor, with the C extractor's options, feature names and flags.
  `VMAF_FEATURE_IMPL=rust` makes every registration path use the twin where
  one exists and logs the C fallback where none does; `--feature psnr_rust`
  picks a twin directly; the JSON report's `feature_backends` names the
  extractor that ran. The C extractors stay the default. The first twin,
  `psnr_rust`, returns the C scores bit for bit on the Netflix pair, both
  checkerboard pairs, a 10-bit pair and 200 frames of 4K.
  `scripts/ci/rust_twin_diff.py` proves a twin equal to its C extractor (same
  binary, equal doubles on every metric of every frame), the `Rust` workflow
  runs clippy on every workspace crate, checks the cbindgen header and runs the
  new `rust` Meson suite. See
  [Rust extractor framework](docs/development/rust-extractor-framework.md).

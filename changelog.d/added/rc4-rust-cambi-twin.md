- **`cambi` runs in Rust, bit-identical to the C extractor (`cambi_rust`).**
  A build with `-Denable_rust_features=true` registers `cambi_rust`, a
  statement-by-statement port of the scalar `cambi.c` path (preprocessing,
  spatial mask, mode filter, sliding-histogram c-values, top-K pooling and the
  luminance model). It reads the C extractor's option table and implements
  every option except `heatmaps_path`, which it refuses at init. Its per-frame
  scores equal the C extractor's at `--precision max` on the Netflix 576x324
  pair, both 1080p checkerboard pairs, the 10-bit sparks pair and BBB
  3840x2160, for the default options and the `vmaf_v1.0.16` models' options.
  Select it with `--feature cambi_rust` or `VMAF_FEATURE_IMPL=rust`; the C
  extractor stays the default. See
  [the CAMBI page](docs/metrics/cambi.md#rust-twin) and
  [ADR-1713](docs/adr/1713-rc4-rust-extractor-framework.md).

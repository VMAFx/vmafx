## Added

- **Percentile temporal pooling in the public C API** (ADR-1188, closes the
  `T-UPSTREAM-818-POOLING-ENUM-NO-PERCENTILES-2026-09-03` ledger row for
  upstream `Netflix/vmaf#818`):
  - `enum VmafPoolingMethod` gains `VMAF_POOL_METHOD_MEDIAN`, `_PERC5`,
    `_PERC10` and `_PERC20`, appended after `HARMONIC_MEAN` so every existing
    discriminant keeps its value. `VMAF_POOL_METHOD_NB` moves 5 → 9; it is a
    count sentinel, not a stable value. A new
    `VMAF_HAVE_PERCENTILE_POOLING` feature macro lets integrations detect the
    methods with `#ifdef` instead of a version comparison.
  - `vmaf_score_pooled` / `vmaf_feature_score_pooled` /
    `vmaf_score_pooled_model_collection` accept the new methods. They sort the
    pooled per-frame scores and interpolate linearly between ranks — exactly
    `numpy.percentile(method="linear")`, the rule the Python harness already
    applies through `ListStats.perc10` — so the C API and the harness report
    the same pooled number for the same frames. Previously a C caller could
    not request a percentile at all: any discriminant past `HARMONIC_MEAN` was
    rejected with `-EINVAL`.
  - Rust binding: `PoolingMethod::{Median, Perc5, Perc10, Perc20}`.
  - FFmpeg: `ffmpeg-patches/0018-libvmaf-map-percentile-pool-methods.patch`
    maps the `pool=median|perc5|perc10|perc20` option strings on every
    `libvmaf*` filter variant, and finally maps `pool=max`, which the enum has
    always had but FFmpeg's mapper never accepted.
  - Docs: [`docs/api/index.md`](docs/api/index.md#vmafpoolingmethod),
    [`docs/usage/ffmpeg.md`](docs/usage/ffmpeg.md),
    [`docs/reference/faq.md`](docs/reference/faq.md).

  Only the new methods allocate: the accumulator methods (`MIN` / `MAX` /
  `MEAN` / `HARMONIC_MEAN`) keep running in constant space with byte-identical
  arithmetic, so the Netflix golden gate is untouched (ADR-1118 isolation).
  Percentiles are order statistics and therefore ignore ADR-1118 perceptual
  weighting, as `MIN` and `MAX` already do. `pooled_metrics` in XML / JSON
  reports still carries exactly `min`, `max`, `mean`, `harmonic_mean`.

<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1181: Percentile Pooling Methods (Median, Perc5, Perc10, Perc20)

- **Status**: Accepted
- **Date**: 2026-09-05
- **Deciders**: Lusoris Maintainers
- **Tags**: `core`, `api`, `pooling`

## Context

Upstream Netflix/vmaf issue #818 requested percentile pooling methods (specifically median, 5th, 10th,
and 20th percentiles) to evaluate outlier frame quality over a video sequence without being masked by
arithmetic or harmonic means. While the Python quality runner harness (`python/vmaf/core/quality_runner.py`)
supported percentile pooling via Python, `libvmaf`'s public C API only provided `MIN`, `MAX`, `MEAN`,
and `HARMONIC_MEAN`.

Callers in C, Go, and FFmpeg filter graphs (`vf_libvmaf`) had no library-native mechanism to compute
percentile pooled scores. Adding these methods requires preserving strict ABI stability (append-only
enumerator additions before `VMAF_POOL_METHOD_NB`), preserving bit-exact Netflix CPU golden-data parity
for existing pooling strategies, and matching the numerical interpolation used by the reference Python runner.

## Decision

We append `VMAF_POOL_METHOD_MEDIAN`, `VMAF_POOL_METHOD_PERC5`, `VMAF_POOL_METHOD_PERC10`, and
`VMAF_POOL_METHOD_PERC20` to `enum VmafPoolingMethod` in `libvmaf.h`.

We extract the shared linear-interpolation `percentile()` and `score_compare()` routines into
`core/src/pooling_percentile.h`, and evaluate percentiles directly within `vmaf_feature_score_pooled`
by sorting the frame scores. Percentile pooling intentionally ignores perceptual spatial weighting
(consistent with `MIN` and `MAX`, as rank order and quantile percentiles operate on unweighted score
distributions).

We expose the new pooling methods across all consumption layers:

1. `core/src/output.cpp`: add string names (`"median"`, `"perc5"`, `"perc10"`, `"perc20"`) and bump compile-time guard `VMAF_POOL_METHOD_NB == 9`.
2. `pkg/libvmaf/`: define Go `PoolMethod` enum, conversion helpers, and wire into `ScoreDirectRequest` and `StreamConfig`.
3. `ffmpeg-patches/`: update `pool_method_map` in patch `0005` (and AVOption descriptions in `0005`, `0006`, `0013`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Option A (Chosen)**: Append named enumerators (`MEDIAN`, `PERC5`, `PERC10`, `PERC20`) to `enum VmafPoolingMethod` | Fully backwards-compatible ABI; zero signature changes; simple usage across C, Go, FFmpeg | Fixed set of percentiles | Directly satisfies Netflix#818 requirements without breaking existing public API signatures |
| **Option B**: Add a generic `vmaf_score_pooled_percentile` API accepting arbitrary `double percentile` | Allows arbitrary percentiles (e.g. 1st, 99th) | Introduces parallel API entry points; does not map cleanly into `VmafPoolingMethod` enum or existing FFmpeg option parser | Over-engineered; callers primarily require 5th, 10th, 20th percentiles and median |
| **Option C**: Implement percentiles exclusively in CLI tooling | No core C library change | Inaccessible to FFmpeg, Go bindings, and downstream C API consumers | Fails library reusability and feature parity across SDK surfaces |

## Consequences

- **Positive**: Native C library support for percentile pooling matching Python runner oracles (PERC10 agrees within $10^{-2}$ with `quality_runner_test.py:680` on `src01_hrc00/01`); zero drift on existing golden-data assertions.
- **Negative**: Small dynamic allocation ($O(N)$ doubles for $N$ frames in pooling window) during percentile evaluation in `vmaf_feature_score_pooled`.
- **Neutral / follow-ups**: Updated API documentation in `docs/api/index.md`; unit tests in `core/test/test_pooling_percentile.c`.

## References

- `req`: "Part A (Netflix/vmaf#818): Percentile pooling methods (VMAF_POOL_METHOD_MEDIAN, PERC5, PERC10, PERC20) appended to enum VmafPoolingMethod in libvmaf.h/libvmaf.c/output.cpp, Go bindings (pkg/libvmaf/), FFmpeg filter patches (0005, 0006, 0013), unit test core/test/test_pooling_percentile.c."
- Netflix/vmaf#818: Feature request for percentile pooling in libvmaf
- ADR-0119: CLI score formatting precision contract

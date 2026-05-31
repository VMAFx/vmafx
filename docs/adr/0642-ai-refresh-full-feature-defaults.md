<!-- markdownlint-disable MD013 MD060 -->
# ADR-0642: AI refresh defaults use current fork full-feature extractors

- **Status**: Accepted
- **Date**: 2026-05-20
- **Deciders**: lusoris
- **Tags**: `ai`, `training-data`, `full-features`, `konvid`, `ugc`, `bvi-dvc`, `fork-local`

## Context

The fork has changed feature extraction, encoder adapters, and corpus handling enough that
old AI feature tables are stale. Several refresh scripts still defaulted to ambiguous
`vmaf` binaries (`build/tools/vmaf`, `/usr/local/bin/vmaf`, or caller PATH), which can miss
fork-only extractors and silently produce incomplete tables. The KoNViD-1k path only had the
legacy pair extractor, UGC refresh still emitted canonical-6 rows with NaN-filled columns,
and aggregate full-feature tables were rebuilt by ad hoc concatenation.

## Decision

We will make the current fork CPU binary (`core/build-cpu/tools/vmaf`) the shared default
for AI feature extraction, add real full-feature refresh drivers for KoNViD-1k and UGC,
allow BVI-DVC refreshes from the known-good lossless MKV bundle, and require aggregate
training tables to be rebuilt through `ai/scripts/combine_full_feature_parquets.py`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep each script's historical default binary | No migration work | Stale installed binaries can lack fork-only extractors; refreshes become non-reproducible across host/container contexts | The refresh must regenerate against current fork behavior |
| Require callers to pass `--vmaf-bin` every time | Explicit and flexible | Easy to forget in long background jobs; scripts keep unsafe defaults | Explicit override stays supported, but the default should be safe |
| Keep UGC canonical-6 with NaN-filled feature columns | Fastest path for old v5 experiments | Produces mixed-schema aggregate tables and stale model inputs | Current FR regressors consume `FULL_FEATURES`; refresh scripts must emit that schema |
| Combine refreshed parquets in notebooks | Flexible one-off analysis | Recreates schema drift and unreviewed ordering choices | A checked-in combiner gives tests, docs, and a stable normalized schema |

## Consequences

- **Positive**: Netflix, KoNViD, BVI-DVC, UGC, and CHUG refresh jobs now have one binary
  contract and one aggregate schema. Folded KoNViD output becomes reproducible across
  machines because fold labels are deterministic hashes over clip keys.
- **Negative**: Full-feature UGC/KoNViD refreshes are slower than the old shortcuts, and
  local jobs need a current `core/build-cpu/tools/vmaf` build before running.
- **Neutral / follow-ups**: Model retraining waits for the long-running refresh jobs
  (CHUG, KoNViD, UGC, Netflix) to finish. The combiner intentionally writes gitignored
  parquet outputs; model-card number updates belong in the follow-up retrain PR.

## References

- Research digest: [AI full-feature refresh defaults](../research/0642-ai-refresh-full-feature-defaults.md).
- [ADR-0026](0026-cross-metric-feature-fusion.md) — full-feature table motivation.
- [ADR-0340](0340-multi-corpus-aggregation.md) — multi-corpus aggregation.
- [ADR-0362](0362-k150k-corpus-integration.md) — existing K150K full-feature precedent.
- Source: `req` — "i wasnt talking about chug only, we bugfixed so many things, all our ai things must be stale"
- Source: `req` — "so all, netflix, regressors, encoders etc... everything we did so far needs updates"

<!-- markdownlint-disable MD060 -->
# ADR-0668: AI Derived Table Provenance

- **Status**: Proposed
- **Date**: 2026-05-21
- **Deciders**: Lusoris maintainers
- **Tags**: ai, training, provenance, parquet

## Context

The AI refresh pipeline now produces multiple local FULL_FEATURES parquet
tables before any model export happens: K150K/FR-from-NR extraction, merged
multi-corpus tables, and post-hoc CHUG/K150K metadata enrichment. Those files
are gitignored and can be large, so the repository cannot rely on committed
bytes to prove which inputs, feature schema, backend split, or command-line
arguments produced them.

ADR-0661 introduced `aiutils.run_manifest.build_run_provenance()` for
training/export JSON sidecars. The remaining gap is the table-building layer
itself. If refreshed tables are anonymous, later HDR/MOS regressors and
`vmaf-tune` encoder profiles can inherit stale or unreplayable data even when
their own trainer manifests are correct.

## Decision

Operator-facing scripts that create or reshape refreshed FULL_FEATURES parquet
tables must emit a JSON manifest sidecar by default:

- `ai/scripts/extract_k150k_features.py` writes
  `<out>.manifest.json` with feature order, extractor split, backend worker
  counts, restart counters, parquet row count, and `run_provenance`.
- `ai/scripts/combine_full_feature_parquets.py` writes
  `<out>.manifest.json` with per-input labels, row counts, missing-feature
  fill lists, aggregate corpus distribution, output column order, and
  `run_provenance`.
- `ai/scripts/enrich_k150k_parquet_metadata.py` writes
  `<out>.manifest.json` with metadata match/update counters, available
  metadata keys, overwrite policy, and `run_provenance`.

Each script accepts `--manifest-out` for experiment bundles that keep the
manifest somewhere other than the default sibling path. Existing parquet row
schemas remain unchanged.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Sidecar manifests on each derived table | Replayable local artifacts; reuses ADR-0661 helper; no parquet schema churn | Adds one small JSON file per run | Chosen; it closes the evidence gap without touching model inputs. |
| Store provenance inside parquet metadata | Keeps one file per artifact | Harder to inspect with standard tools; many local scripts rewrite parquet through pandas and may drop custom metadata | Rejected; human-readable JSON is easier to audit and preserve. |
| Only document the commands in `.workingdir2` notes | No code change | Notes drift from actual invocations and are not consumed by trainer/model-card tooling | Rejected; the scripts must stamp the artifact they create. |
| Defer until the K150K/CHUG runs finish | Avoids touching scripts during a long refresh | The finished table would still be unreplayable and every downstream model would inherit that gap | Rejected; provenance should be in place before the next full refresh table is trusted. |

## Consequences

- **Positive**: Refreshed AI training tables can be traced back to inputs,
  args, output targets, feature schema, and row counts without shell history.
- **Positive**: Metadata-enriched CHUG/K150K parquet files can be distinguished
  from freshly extracted tables in later HDR model cards.
- **Negative**: Local runs create an additional small JSON sidecar that
  operators need to keep with the parquet artifact.
- **Neutral / follow-ups**: Future table builders should adopt the same helper
  before they become operator-facing; no existing model input column order
  changes in this ADR.

## References

- [ADR-0661](0661-ai-run-manifest-provenance.md)
- [docs/ai/training.md](../ai/training.md)
- Source: req "batch things that are connected and create them"
- Source: req "everything we did so far needs updates"

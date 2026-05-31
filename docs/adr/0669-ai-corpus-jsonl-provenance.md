<!-- markdownlint-disable MD060 -->
# ADR-0669: AI Corpus JSONL Provenance

- **Status**: Proposed
- **Date**: 2026-05-21
- **Deciders**: Lusoris maintainers
- **Tags**: ai, training, provenance, corpus

## Context

ADR-0661 standardised run provenance for durable AI JSON reports and model
sidecars. ADR-0668 extended that convention to refreshed FULL_FEATURES parquet
tables. The remaining training-input gap is one layer earlier: corpus JSONL
files that are merged or aggregated before trainers see them.

`ai/scripts/aggregate_corpora.py` normalises MOS-labelled corpora onto a
shared 0-100 target axis. `ai/scripts/merge_corpora.py` combines vmaf-tune
Phase-A encode-grid corpora for FR-regressor training. Both create local,
gitignored JSONL artifacts that may be retained for later training runs, but
previously did not stamp which input shards, scale conversions, dedup policy,
or command line produced them.

## Decision

The corpus JSONL boundary scripts emit JSON manifest sidecars by default:

- `aggregate_corpora.py` writes `<output>.manifest.json` with aggregate
  counters, scale-conversion metadata, optional corpus-source overrides, and
  shared `run_provenance`.
- `merge_corpora.py` writes `<output>.manifest.json` with summary counters,
  required schema keys, natural-key dedup fields, and shared
  `run_provenance`.

Both scripts accept `--manifest-out` when an experiment bundle needs a
different sidecar path. Existing JSONL row schemas remain unchanged.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Default sibling manifests | Replayable corpus artifacts; no row-schema churn; reuses ADR-0661 helper | Adds one small JSON file per run | Chosen; this closes the durable-input evidence gap with low blast radius. |
| Add provenance fields to every JSONL row | Self-contained rows | Bloats large corpora and repeats identical run metadata thousands of times | Rejected; sidecars are clearer for run-level facts. |
| Only stamp trainer manifests | No corpus-script changes | A trainer manifest can identify a merged JSONL path but not prove how that JSONL was produced | Rejected; the input artifact itself must be replayable. |
| Wait for every per-corpus adapter to emit manifests | More complete ingest coverage | Delays the shared merge/aggregate boundary and creates a very large PR | Rejected for this batch; adapter-level manifests can follow separately. |

## Consequences

- **Positive**: Training JSONL artifacts now carry input shard hashes, parsed
  arguments, output paths, schema/dedup policy, and aggregate counters.
- **Positive**: Model-card evidence can cite a corpus merge manifest rather
  than relying on `.workingdir2` notes or shell history.
- **Negative**: Operators need to keep one extra JSON file next to merged or
  aggregated corpus JSONL outputs.
- **Neutral / follow-ups**: Raw per-corpus adapter manifests are still useful
  and can adopt the same pattern in later PRs without changing this contract.

## References

- [ADR-0661](0661-ai-run-manifest-provenance.md)
- [ADR-0668](0668-ai-derived-table-provenance.md)
- [docs/ai/mos-corpora.md](../ai/mos-corpora.md)
- Source: req "batch things that are connected and create them"
- Source: req "everything we did so far needs updates"

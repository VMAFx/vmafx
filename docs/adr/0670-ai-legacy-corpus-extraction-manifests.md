<!-- markdownlint-disable MD060 -->
# ADR-0670: AI Legacy Corpus Extraction Manifests

- **Status**: Proposed
- **Date**: 2026-05-21
- **Deciders**: Lusoris maintainers
- **Tags**: ai, training, provenance, corpus

## Context

ADR-0661 made `run_provenance` the shared evidence block for durable AI
artifacts. ADR-0668 covered refreshed FULL_FEATURES table builders, and
ADR-0669 covered corpus JSONL merge/aggregate boundaries. The older extraction
scripts still left gaps:

- `extract_full_features.py` writes the Netflix public FULL_FEATURES parquet.
- `konvid_to_vmaf_pairs.py` writes the synthetic-distortion KoNViD-1k FR-pair
  parquet consumed by the LOSO trainer.
- `bvi_dvc_to_corpus_jsonl.py` reshapes cached BVI-DVC libvmaf JSON into
  vmaf-tune corpus rows for FR-regressor training.

Those outputs are local, gitignored, and often expensive to recreate. Without
sidecars they are anonymous training inputs: a later model card can identify a
path, but not the exact corpus/cache root, feature list, CRF, row count,
failed-clip count, or command line that created the artifact.

## Decision

The legacy corpus/extraction scripts emit replay manifests by default:

- `extract_full_features.py --manifest-out` defaults to
  `<out>.manifest.json` and records the Netflix corpus root, cache root, VMAF
  binary, feature list, pair count, row count, and shared `run_provenance`.
- `konvid_to_vmaf_pairs.py --manifest-out` defaults to
  `<out>.manifest.json` and records KoNViD root, model/VMAF inputs, cache
  policy, CRF, feature list, selected/processed/failed clip counts, failed clip
  IDs, frame count, and shared `run_provenance`.
- `bvi_dvc_to_corpus_jsonl.py --manifest-out` defaults to
  `<output>.manifest.json` and records cache inputs, adapter labels, row schema
  version, row count, cache-file count, and shared `run_provenance`.

The scripts keep their existing output schemas. While touching BVI-DVC JSONL,
the adapter also emits the current vmaf-tune v3 additive columns with explicit
unavailable defaults so stale cached BVI rows no longer fail the current
`CORPUS_ROW_KEYS` contract.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Default sibling manifests | Replayable expensive artifacts; keeps row schemas stable; matches ADR-0668/0669 | Adds one small JSON file per run | Chosen; it closes the evidence gap with low operational cost. |
| Append provenance columns/rows to the parquets and JSONL | Self-contained output files | Changes trainer-facing schemas and repeats run metadata per row | Rejected; run-level evidence belongs in sidecars. |
| Only document operator commands | No code change | Still loses input hashes, parsed arguments, row counts, and failure counters | Rejected; docs are not evidence for local artifacts. |
| Wait for K150K refresh completion | Avoids touching old scripts mid-run | Leaves immediately usable Netflix/KoNViD/BVI artifacts anonymous | Rejected; these scripts are independent of the running K150K job. |

## Consequences

- **Positive**: Legacy Netflix, KoNViD, and BVI training-input artifacts now
  carry replayable path/hash/argument evidence.
- **Positive**: BVI-DVC JSONL rows are brought back into current vmaf-tune v3
  schema compliance with explicit missing-signal defaults.
- **Negative**: Operators must keep one extra JSON sidecar next to each local
  corpus/extraction output.
- **Neutral / follow-ups**: Other acquisition-only download manifests can adopt
  the same pattern later, but this ADR covers the older scripts that directly
  create trainer inputs.

## References

- [ADR-0661](0661-ai-run-manifest-provenance.md)
- [ADR-0668](0668-ai-derived-table-provenance.md)
- [ADR-0669](0669-ai-corpus-jsonl-provenance.md)
- [docs/ai/training.md](../ai/training.md)
- Source: req "go on with next backlog?"
- Source: req "everything we did so far needs updates"

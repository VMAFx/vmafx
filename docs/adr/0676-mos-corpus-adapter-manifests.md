<!-- markdownlint-disable MD013 MD060 -->
# ADR-0676: MOS Corpus Adapter Manifests

- **Status**: Accepted
- **Date**: 2026-05-21
- **Deciders**: Lusoris, Codex
- **Tags**: ai, mos, corpus, provenance, fork-local

## Context

ADR-0661 made AI run provenance a shared schema, ADR-0669 covered corpus JSONL
merge/aggregate outputs, and ADR-0670 covered several legacy trainer-input
builders. The remaining MOS corpus adapters still produced local JSONL shards
without a sibling replay sidecar. That left CHUG, KoNViD, YouTube-UGC, LSVQ,
LIVE-VQC, and Waterloo-IVC rows weaker as model-card evidence than the
downstream tables derived from them.

The gap is operationally risky because these adapters download or resolve
research corpora from operator-local paths, apply max-row caps, tolerate
download attrition, and emit corpus-specific MOS scale fields. A later trainer
cannot infer those choices from the JSONL rows alone.

## Decision

All MOS corpus JSONL adapters using `corpus.base.CorpusIngestBase`, plus the
KoNViD-1k local adapter, will write `<output>.manifest.json` by default and
accept `--manifest-out PATH` for experiment bundles. The manifest records the
corpus label, run counters, effective ingest config, path inputs/outputs, and
ADR-0661 `run_provenance`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Shared base helper plus per-adapter `--manifest-out` | One schema path; consistent counters/provenance; minimal per-script code | Touches several CLI docs and tests in one PR | Chosen: it closes the whole adapter family rather than leaving partial evidence |
| Add manifests only to CHUG | Fastest HDR-specific patch | KoNViD/UGC/LSVQ/LIVE/Waterloo shards remain anonymous and inconsistent | Rejected: MOS-head refreshes combine multiple corpora |
| Rely on downstream merge manifests | No new CLI flags | Merge manifests cannot prove download attrition, max-row caps, source roots, or corpus-specific parser config | Rejected: source-adapter choices are lost before merge |
| Embed run metadata into every JSONL row | Single artifact | Repeats run-level metadata per row and changes trainer row shape | Rejected: row schemas should remain stable; run evidence belongs in a sidecar |

## Consequences

- **Positive**: MOS corpus JSONL shards can be cited by trainers, signal-mix
  audits, and model cards with replayable input/output evidence.
- **Negative**: Any new MOS adapter CLI must document and test its manifest
  sidecar in addition to row schema.
- **Neutral / follow-ups**: Regenerate local CHUG/KoNViD/UGC/LSVQ/LIVE/Waterloo
  JSONL shards with sidecars before using them in a promoted model-card
  refresh.

## References

- [ADR-0661](0661-ai-run-manifest-provenance.md) — shared AI run provenance.
- [ADR-0669](0669-ai-corpus-jsonl-provenance.md) — corpus JSONL aggregate and
  merge manifests.
- [ADR-0670](0670-ai-legacy-corpus-extraction-manifests.md) — legacy corpus
  extraction manifests.
- Source: req — "so all, netflix, regressors, encoders etc... everything we did so far needs updates"
- Source: req — "well go on i guess we have enough backlog..."

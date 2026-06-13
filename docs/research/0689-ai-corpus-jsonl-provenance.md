# Research Digest 0689 — AI corpus JSONL provenance

## Scope

Audit the AI scripts that create durable corpus JSONL inputs after raw corpus
ingestion and before trainer/model sidecars are written.

## Findings

- `aggregate_corpora.py` is the MOS-labelled corpus boundary: it rescales
  KonViD/LSVQ/YouTube/LIVE/Waterloo-style MOS shards and chooses cross-corpus
  duplicate winners.
- `merge_corpora.py` is the vmaf-tune encode-grid corpus boundary: it combines
  Netflix/BVI-style rows and deduplicates by `(src_sha256, encoder, preset,
  crf)`.
- Both outputs are gitignored training inputs. A later trainer manifest could
  name the merged JSONL path but could not explain which source shards,
  conversion policy, dedup policy, or command produced that path.
- ADR-0661 already provides the shared `run_provenance` helper, so a sidecar
  manifest is the lowest-risk fix and avoids changing row schemas.

## Implementation Notes

- Add `--manifest-out` to both scripts and default to
  `<output>.manifest.json`.
- Keep JSONL row bytes compatible with existing trainers.
- Record aggregate counters, schema/dedup policy, scale conversions, and corpus
  overrides next to the shared provenance block.
- Unit-test the CLI paths against synthetic JSONL shards only; no real dataset
  bytes are required.

## Validation Plan

- Run `ai/tests/test_aggregate_corpora.py` and `ai/tests/test_merge_corpora.py`.
- Run Ruff and Black over the touched scripts/tests.
- Run ADR numbering, mkdocs strict, format-check, diff-check, and the
  ADR-0108 deliverables gate with the PR body.

## References

- [ADR-0661](../adr/0661-ai-run-manifest-provenance.md)
- [ADR-0668](../adr/0668-ai-derived-table-provenance.md)
- [ADR-0669](../adr/0669-ai-corpus-jsonl-provenance.md)
- [docs/ai/mos-corpora.md](../ai/mos-corpora.md)

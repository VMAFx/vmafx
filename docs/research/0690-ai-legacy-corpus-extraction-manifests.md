# Research Digest 0690 — AI legacy corpus extraction manifests

## Scope

Audit older AI extraction/adaptation scripts that directly create trainer-input
parquets or vmaf-tune corpus JSONL outside the newer ADR-0668/0669 manifest
coverage.

## Findings

- `extract_full_features.py` writes the Netflix public FULL_FEATURES parquet
  and previously only printed the output row count.
- `konvid_to_vmaf_pairs.py` writes the KoNViD-1k synthetic-distortion FR-pair
  parquet and could skip failed clips without a durable failed-clip list.
- `bvi_dvc_to_corpus_jsonl.py` writes vmaf-tune corpus JSONL from cached
  BVI-DVC libvmaf JSON. The adapter also lagged the current vmaf-tune v3 row
  schema, so current `CORPUS_ROW_KEYS` included HDR, shot, canonical-feature,
  and encoder-internal columns the adapter never populated.
- All three artifacts are gitignored and expensive enough that shell history is
  not an adequate replay contract.

## Implementation Notes

- Add `--manifest-out` to all three scripts, defaulting to a sibling
  `<out>.manifest.json` / `<output>.manifest.json`.
- Reuse `aiutils.run_manifest.build_run_provenance()` and
  `write_manifest_json()`; do not invent script-specific path schemas.
- Keep parquet and JSONL schemas stable except for fixing the BVI-DVC adapter to
  produce the already-current vmaf-tune v3 row keys.
- Unit-test with synthetic caches and monkeypatched extraction functions so no
  real dataset, ffmpeg, or libvmaf binary is needed.

## Validation Plan

- Run `ai/tests/test_legacy_corpus_extraction_manifests.py`.
- Run Ruff and Black over the touched scripts/tests.
- Run ADR numbering, mkdocs strict, format-check, diff-check, and the
  ADR-0108 deliverables gate with the PR body.

## References

- [ADR-0661](../adr/0661-ai-run-manifest-provenance.md)
- [ADR-0668](../adr/0668-ai-derived-table-provenance.md)
- [ADR-0669](../adr/0669-ai-corpus-jsonl-provenance.md)
- [ADR-0670](../adr/0670-ai-legacy-corpus-extraction-manifests.md)
- [docs/ai/training.md](../ai/training.md)

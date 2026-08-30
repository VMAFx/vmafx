# Research Digest 0688 — AI derived table provenance

## Scope

Audit the AI refresh scripts that create durable FULL_FEATURES parquet tables
before model training/export, and close the remaining provenance gap from
ADR-0661.

## Findings

- `aiutils.run_manifest.build_run_provenance()` already gives training and
  report sidecars a stable shape for entrypoint, argv, parsed args, inputs, and
  outputs.
- `extract_k150k_features.py` produced large local parquet files plus `.done`
  and `.rows.jsonl` restart files, but no durable manifest tying the final table
  to the feature schema, backend split, restart counters, or input sidecars.
- `combine_full_feature_parquets.py` normalised and concatenated refreshed
  corpus shards, but the output parquet did not record which shards were used
  or which feature columns were filled with `NaN`.
- `enrich_k150k_parquet_metadata.py` can turn an existing CHUG/K150K parquet
  into a different training artifact by adding split/content metadata, but the
  enriched output did not record match counts or the sidecar used.
- All three scripts are operator-facing and produce gitignored training
  evidence, so relying on `.workingdir2` notes or shell history is too fragile
  for later model cards and tune-profile proofs.

## Implementation Notes

- Add `--manifest-out` to all three scripts and default it to
  `<out>.manifest.json`.
- Keep parquet schemas unchanged; the manifest is a sibling JSON artifact.
- Use the shared ADR-0661 helper instead of bespoke argument/path JSON.
- Record script-specific replay facts next to the shared block:
  K150K extractor backend/features/restart counters, combiner input rows and
  missing-feature fills, and metadata enricher match/update counters.
- In the K150K extractor, recover surviving `.rows.jsonl` staging rows even
  when there are no pending clips, then write the manifest. This closes the
  interrupted-final-write edge case that otherwise leaves a complete `.done`
  checkpoint without a materialized parquet.

## Validation Plan

- Unit-test K150K manifest emission directly against a tiny parquet.
- Unit-test combiner manifest content through its CLI path.
- Unit-test metadata enrichment manifest content through the subprocess CLI
  smoke test.
- Keep the focused tests independent from real corpus bytes.

## References

- [ADR-0661](../adr/0661-ai-run-manifest-provenance.md)
- [ADR-0668](../adr/0668-ai-derived-table-provenance.md)
- [docs/ai/training.md](../ai/training.md)
- [docs/ai/chug-ingestion.md](../ai/chug-ingestion.md)

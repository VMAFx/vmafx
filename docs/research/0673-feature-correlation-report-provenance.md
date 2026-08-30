<!-- markdownlint-disable MD060 -->
# Research 0673 — Feature-correlation report provenance

## Summary

The signal-mix audit backlog uses `ai/scripts/feature_correlation.py` as one of
the older per-corpus analysis tools. It wrote a durable JSON report with the
Pearson matrix, mutual-information scores, LASSO / random-forest importances,
and consensus top-K list, but it did not record the command, source parquet, or
ranking parameters in ADR-0661 `run_provenance`.

## Files audited

- `ai/scripts/feature_correlation.py`
- `ai/tests/test_feature_correlation.py`
- `docs/research/0027-phase2-feature-importance.md`
- `docs/ai/training.md`
- `docs/adr/0661-ai-run-manifest-provenance.md`

## Findings

- The script is a user-facing analysis entrypoint and its `--out` JSON is used
  as durable research evidence.
- The existing shared `aiutils.run_manifest` helper fits the report without a
  new schema: the replay-critical inputs are one parquet file, parsed ranking
  options, original argv, and the output JSON path.
- The existing synthetic parquet test already exercises the full CLI path, so
  it can assert the provenance block without introducing expensive feature
  extraction.

## Decision matrix

| Option | Benefits | Costs | Decision |
|---|---|---|---|
| Leave the report as plain JSON | No code churn | Report cannot prove which parquet / thresholds produced a ranking | Rejected |
| Add an ad hoc `source` object | Small local diff | Duplicates ADR-0661 path hashing and argument normalization | Rejected |
| Attach ADR-0661 `run_provenance` | Shared schema; hashes the source parquet; records argv and ranking parameters | Slightly larger report JSON | Chosen |

## Outcome

`feature_correlation.py --out` now writes through `write_manifest_json()` and
includes `run_provenance` with the analyzer entrypoint, parsed arguments,
source parquet, and JSON report target.

## Validation

- `.venv/bin/ruff check ai/scripts/feature_correlation.py ai/tests/test_feature_correlation.py`
- `.venv/bin/python -m pytest ai/tests/test_feature_correlation.py -q`

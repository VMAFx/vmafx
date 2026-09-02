<!-- markdownlint-disable MD060 -->
# Research 0674 — Phase-3 subset-sweep report provenance

## Summary

`ai/scripts/phase3_subset_sweep.py` writes model-selection JSON used by
Research-0028/0029/0030 to compare canonical and expanded feature subsets. The
report kept per-subset metrics but not the source parquet, subset list,
standardization setting, seed policy, or training hyperparameters in a durable
ADR-0661 `run_provenance` block.

## Files audited

- `ai/scripts/phase3_subset_sweep.py`
- `ai/tests/test_phase3_subset_sweep.py`
- `docs/research/0028-phase3-subset-sweep.md`
- `docs/ai/training.md`
- `docs/adr/0661-ai-run-manifest-provenance.md`

## Findings

- The output is gitignored but durable research evidence; it is referenced by
  model cards and Research-0028/0029/0030.
- The replay-critical context is the source parquet plus parsed sweep
  arguments, not an environment dump.
- A lightweight test can monkeypatch the expensive LOSO fold trainer and still
  exercise the CLI report write path.

## Decision matrix

| Option | Benefits | Costs | Decision |
|---|---|---|---|
| Leave the report as subset-key JSON only | No compatibility change | Cannot replay a stored sweep without shell history | Rejected |
| Add a bespoke `_metadata` block | Keeps report local | Duplicates ADR-0661 schema and path hashing | Rejected |
| Add top-level ADR-0661 `run_provenance` | Shared replay schema; records parquet, argv, and sweep args | Adds one top-level key next to subset results | Chosen |

## Outcome

`phase3_subset_sweep.py --out` now writes through `write_manifest_json()` and
adds top-level `run_provenance` to the report.

## Validation

- `.venv/bin/ruff check ai/scripts/phase3_subset_sweep.py ai/tests/test_phase3_subset_sweep.py`
- `.venv/bin/python -m pytest ai/tests/test_phase3_subset_sweep.py -q`

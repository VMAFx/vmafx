<!-- markdownlint-disable MD060 -->
# Research-0671: Ensemble LOSO Report Provenance

## Summary

The ADR-0661 provenance sweep covered model sidecars, evaluation reports,
materializer audits, ensemble validator verdicts, and production seed exports.
The remaining ensemble gap was the source gate input:
`ai/scripts/train_fr_regressor_v2_ensemble_loso.py` wrote `loso_seed{N}.json`
with direct `json.dump()` and no `run_provenance` block.

Those reports are the durable evidence consumed by
`scripts/ci/ensemble_prod_gate.py` and
`ai/scripts/validate_ensemble_seeds.py`. They need to identify the corpus JSONL,
argv, parsed training hyperparameters, and per-seed report target before the
validator aggregates them into `PROMOTE.json` or `HOLD.json`.

## Files Audited

- `ai/scripts/train_fr_regressor_v2_ensemble_loso.py`
- `ai/tests/test_train_fr_regressor_v2_ensemble_loso_train.py`
- `docs/ai/ensemble-v2-real-corpus-retrain-runbook.md`
- `docs/ai/ensemble-training-kit.md`
- ADR-0661 and `aiutils.run_manifest`

## Findings

- The LOSO report schema already carries gate metrics, fold traces, seed,
  corpus path, and training hyperparameters.
- The report did not preserve the original command line or a hashed description
  of the corpus input.
- The existing ADR-0661 helper can describe the corpus and report target without
  changing the validator input shape expected by `ensemble_prod_gate.py`.

## Decision Matrix

| Option | Pros | Cons | Result |
|---|---|---|---|
| Keep LOSO reports as legacy JSON | Smallest diff | Gate inputs remain less traceable than validator verdicts | Rejected |
| Add custom `command` / `paths` fields | Localized schema | Recreates one-off provenance instead of using ADR-0661 | Rejected |
| Attach ADR-0661 `run_provenance` to each `loso_seed{N}.json` | Shared schema; records corpus, argv, args, and report target | Slightly larger reports | Chosen |

## Outcome

`train_fr_regressor_v2_ensemble_loso.py` now builds a `run_provenance` block for
each seed report and writes the report through `write_manifest_json()`. The
report remains backward-compatible for gate consumers because the metric keys
and fold arrays are unchanged.

## Validation

```bash
.venv/bin/ruff check \
  ai/scripts/train_fr_regressor_v2_ensemble_loso.py \
  ai/tests/test_train_fr_regressor_v2_ensemble_loso_train.py

.venv/bin/python -m pytest \
  ai/tests/test_train_fr_regressor_v2_ensemble_loso_train.py -q
```

<!-- markdownlint-disable MD060 -->
# Research 0678 — Ensemble manifest provenance

## Question

ADR-0661 coverage now includes the ensemble LOSO reports, validator verdict,
and production seed exporter, but the older direct
`ai/scripts/train_fr_regressor_v2_ensemble.py` path still wrote
`fr_regressor_v2_ensemble_v1.json` with a bare `json.dumps()` call.
That manifest is the top-level runtime entry point for the deep ensemble, so
it should carry the same replay metadata as the per-seed sidecars.

## Findings

- The direct trainer writes the ensemble manifest after exporting member ONNXs
  and before updating `model/tiny/registry.json`.
- A useful provenance block can be added without changing the manifest keys
  consumed by runtime readers: keep `members`, `confidence`, feature stats, and
  codec vocabulary unchanged, and add top-level `run_provenance`.
- Recording the optional corpus parquet, member ONNX outputs, registry target,
  manifest target, argv, and parsed arguments is enough to replay smoke and
  production manifest refreshes.

## Alternatives considered

| Option | Benefit | Risk | Decision |
|---|---|---|---|
| Leave the direct trainer unchanged | No schema delta | The top-level ensemble manifest stays less reproducible than the newer seed sidecars | Rejected |
| Store custom `training_metadata` | Smaller local diff | Duplicates ADR-0661 normalization and path hashing | Rejected |
| Add top-level ADR-0661 `run_provenance` | Matches the rest of the AI refresh sidecars | Slightly larger manifest JSON | Accepted |

## Validation

- `.venv/bin/ruff check ai/scripts/train_fr_regressor_v2_ensemble.py ai/tests/test_train_fr_regressor_v2_ensemble.py`
- `.venv/bin/python -m pytest ai/tests/test_train_fr_regressor_v2_ensemble.py -q`
- `.venv/bin/mkdocs build --strict`

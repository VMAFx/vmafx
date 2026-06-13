# Research-0665: AI Eval / Validate Report Run Provenance

## Question

After ADR-0661 added shared run provenance to AI training and export sidecars,
should the same schema be used by tiny-VMAF evaluation and validation reports
before the next model refresh wave?

## Findings

- `eval_loso_vmaf_tiny_v3.py`, `eval_loso_vmaf_tiny_v4.py`, and
  `eval_loso_vmaf_tiny_v5.py` write report JSONs that are later cited from
  model cards and research digests, but the report did not record which local
  feature parquet or CLI hyperparameters produced it.
- `eval_multiseed_v3_v4.py` is specifically used to compare architecture
  variance. Without provenance, a multi-seed result can outlive the shell
  command that produced it and become ambiguous after feature refreshes.
- The existing `aiutils.run_manifest` helper already records entrypoint, argv,
  parsed args, named inputs, named outputs, and file hashes where paths exist.
  A separate eval-only schema would recreate the drift ADR-0661 removes.
- Evaluation reports are local artifacts, not shipped model manifests, so the
  block should stay compact and avoid claiming full environment reproducibility.
- `validate_ensemble_seeds.py` writes `PROMOTE.json` / `HOLD.json` verdicts
  that gate model-registry production flips. Those verdicts already snapshot
  corpus contents, but not the validator command, thresholds, seed list, or
  output path in the shared ADR-0661 schema.

## Chosen Follow-Up

Widen ADR-0661 adoption to tiny-VMAF evaluation and validation reports:

- v3 LOSO report records the feature parquet input and `report_target`.
- v4 LOSO report records the feature parquet input and `report_target`.
- v5 corpus-expansion report records both `parquet_base` and `parquet_extra`.
- v3/v4 multi-seed report records the feature parquet input, selected arch,
  seed list, and `report_target`.
- Ensemble-seed validation verdicts record `loso_dir`, `corpus_root`, seed
  list, gate thresholds, and the `PROMOTE.json` / `HOLD.json` output path.

This is a report-provenance change only. It does not retrain weights, change
feature columns, alter model-card scores, or modify Netflix golden assertions.

## Validation

- Unit tests stub the expensive training/eval loops and assert each report
  carries an `ai-run-provenance-v1` block.
- Ensemble-seed validator tests assert the verdict JSON carries the same
  schema and points at the emitted verdict file.
- Ruff checks the touched eval scripts and new tests.
- Existing model-card docs explain where operators should look for eval report
  provenance when comparing refreshed numbers.

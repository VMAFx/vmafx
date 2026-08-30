# Research-0666: Legacy AI Eval Report Run Provenance

## Question

After the refreshed tiny-VMAF eval reports adopted ADR-0661 provenance, which
older AI evaluation reports still leave durable JSON artifacts without a stable
run identity?

## Findings

- `eval_loso_mlp_small.py` and `eval_loso_3arch.py` still produce model-card
  evidence under `runs/loso_eval/`, but their JSON only recorded the corpus
  path and metric payload. The fold checkpoint directory, baseline ONNX inputs,
  original argv, and Markdown report target were implicit.
- `eval_probabilistic_proxy.py` is the reference evaluator for the
  `fr_regressor_v2` ensemble uncertainty surface. Its metrics JSON can be cited
  when calibrating coverage, but the report did not identify the exact ensemble
  manifest or held-out parquet used for the run.
- `eval_saliency_per_mb.py` is the ADR-0396 saliency block-IoU harness. The
  report is useful only when the predicted and ground-truth mask directories are
  preserved with the block size / threshold settings that produced the score.
- These scripts do not need new bespoke schemas. `aiutils.run_manifest` already
  describes entrypoints, argv, parsed args, inputs, outputs, and file hashes
  where available.

## Chosen Follow-Up

Extend ADR-0661 adoption to the legacy eval/report surfaces:

- LOSO `mlp_small` report records `data_root`, `loso_dir`, baseline ONNX files,
  and JSON/Markdown report targets.
- LOSO `3arch` report records `data_root`, `training_runs_dir`, and
  JSON/Markdown report targets.
- Probabilistic proxy metrics record the ensemble manifest, optional parquet,
  and metrics output path.
- Saliency per-block IoU output records predicted/ground-truth mask directories
  and the JSON output path.

This is a report-schema/provenance change only. It does not retrain weights,
change thresholds, alter model-card scores, or modify Netflix golden
assertions.

## Validation

- New unit tests stub the expensive ONNX and corpus paths for the legacy LOSO
  and probabilistic evaluators, then assert `ai-run-provenance-v1` appears in
  the emitted JSON.
- The existing saliency CLI test now verifies the output JSON includes the same
  provenance block.
- Ruff and the focused pytest suite cover the touched scripts and tests.

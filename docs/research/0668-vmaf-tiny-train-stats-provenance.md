# Research-0668: vmaf_tiny Train Stats Run Provenance

## Question

After exporters and evaluation reports adopted ADR-0661 provenance, which
vmaf_tiny artifacts still sit between refreshed parquet tables and ONNX sidecars
without a stable run identity?

## Findings

- `train_vmaf_tiny_v2.py`, `train_vmaf_tiny_v3.py`, and
  `train_vmaf_tiny_v4.py` emit `--out-stats` JSON files containing scaler
  means/stds, training metrics, row counts, and hyperparameters. Those stats
  files feed the corresponding exporters, but they did not record the parquet
  path, argv, checkpoint target, or stats target in the shared ADR-0661 shape.
- `train_vmaf_tiny_v5.py` is deferred for shipping, but its exploratory stats
  file has the same provenance need because v5 compares a base parquet against
  an extra UGC parquet.
- The exporter sidecars already record `run_provenance`, so leaving the training
  stats untagged creates a gap exactly where refreshed AI tables are likely to
  change.

## Chosen Follow-Up

Attach `aiutils.run_manifest.build_run_provenance()` output to each
`--out-stats` JSON and write through `write_manifest_json()`:

- v2/v3/v4 stats record the single parquet input plus checkpoint and stats
  output targets.
- v5 stats record `parquet_base`, `parquet_extra`, checkpoint target, stats
  target, argv, and parsed hyperparameters.

This is a metadata/reporting change only. It does not retrain shipped weights,
change model metrics, alter ONNX export behavior, or modify Netflix golden
assertions.

## Validation

- New tests run each trainer on a tiny synthetic parquet with `--epochs 0` and
  assert the emitted stats JSON contains `schema = ai-run-provenance-v1`, the
  correct entrypoint, argv, input parquet path(s), checkpoint target, and stats
  target.
- Focused Ruff and pytest cover the touched train scripts and new test file.

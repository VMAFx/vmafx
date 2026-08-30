# Research-0667: Predictor v2 Report Run Provenance

## Question

After ADR-0661 covered model sidecars and evaluation reports, which
operator-facing AI report still loses the command/corpus identity needed to
reproduce a failed training gate?

## Findings

- `ai/scripts/train_predictor_v2_realcorpus.py` writes
  `runs/predictor_v2_realcorpus/report.json`, the per-codec evidence consumed by
  the predictor-v2 model-card update wrapper.
- The report already records gate constants, per-codec fold metrics, discovered
  corpus files, and summary counts, but it did not record the trainer entrypoint,
  original argv, parsed CLI arguments, corpus roots, or report target in the
  shared ADR-0661 shape.
- The missing provenance is most visible on honest failures: `--allow-empty`,
  insufficient-source, and gate-failed reports are exactly the artifacts an
  operator uses to decide whether to regenerate corpora, add sources, or keep a
  stub model.
- No bespoke schema is needed. `aiutils.run_manifest.build_run_provenance()`
  already covers entrypoints, argv, parsed args, named inputs, and output
  targets without making gitignored corpus paths part of a CI contract.

## Chosen Follow-Up

Attach the shared `run_provenance` block to the predictor-v2 real-corpus JSON
report and write it through `aiutils.run_manifest.write_manifest_json()`.

The block records:

- `ai/scripts/train_predictor_v2_realcorpus.py` as the user-facing entrypoint.
- The exact argv and parsed arguments, including codec filters, epochs, seed,
  `--allow-empty`, and report target.
- Explicit corpus files, configured corpus roots, and resolved JSONL files.
- The report output target.

This is a report-schema/provenance change only. It does not retrain predictor
weights, lower ADR-0303 gates, overwrite ONNX stubs, or modify Netflix golden
assertions.

## Validation

- A new unit test runs the `--allow-empty` diagnostic path against an empty
  corpus root and asserts the emitted report contains
  `schema = ai-run-provenance-v1`, the trainer entrypoint, argv, corpus roots,
  resolved corpus files, and report target.
- The existing predictor-v2 gate tests still pin ADR-0303 constants and LOSO
  source partitioning.

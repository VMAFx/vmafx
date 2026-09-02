# Ensemble Training Kit

> **Status**: Shipped packaging surface. The portable
> `tools/ensemble-training-kit/` bundle is in tree, accepted by
> [ADR-0324](../adr/0324-ensemble-training-kit.md), and includes the
> numbered step scripts, one-command orchestrator, tarball builder,
> corpus extraction helper, frozen Python requirements, and shell smoke
> tests.

The *ensemble training kit* (per
[ADR-0324](../adr/0324-ensemble-training-kit.md)) is a portable
bundle of the Phase-A corpus producer + LOSO retrain runbook used
to land a second-hardware verdict on the
`fr_regressor_v2_ensemble_v1` retrain. The kit collects the
already-in-tree pieces:

- `scripts/dev/hw_encoder_corpus.py` — Phase-A producer.
- `ai/scripts/run_ensemble_v2_real_corpus_loso.sh` — LOSO wrapper.
- `ai/scripts/train_fr_regressor_v2_ensemble_loso.py` — trainer.
- `ai/scripts/validate_ensemble_seeds.py` — gate verdict emitter.
- `ai/scripts/export_ensemble_v2_seeds.py` — per-seed ONNX exporter.
- `tools/ensemble-training-kit/` — the portable bundle entry-point
  (binaries + tests).
- [`docs/ai/ensemble-v2-real-corpus-retrain-runbook.md`](ensemble-v2-real-corpus-retrain-runbook.md)
  — the operator runbook.

Status: **Accepted / shipped**. ADR-0324 was accepted during the
2026-05-08 ADR status sweep, and the on-disk bundle exists under
`tools/ensemble-training-kit/`. Treat this page as the overview for the
release-facing package; the detailed operator runbook lives in
[`tools/ensemble-training-kit/README.md`](../../tools/ensemble-training-kit/README.md).

## What The Kit Packages

The kit is a convenience wrapper around the real training flow. It does
not define a new model format and does not replace the LOSO trainer.

| Step | Existing entry point | Output |
| --- | --- | --- |
| Capture hardware corpus | `scripts/dev/hw_encoder_corpus.py` | Phase-A JSONL rows |
| Train LOSO folds | `ai/scripts/run_ensemble_v2_real_corpus_loso.sh` | fold metrics + checkpoints |
| Validate seeds | `ai/scripts/validate_ensemble_seeds.py` | `PROMOTE.json` verdict |
| Export seed ONNXs | `ai/scripts/export_ensemble_v2_seeds.py` | `fr_regressor_v2_ensemble_v1_seed*.onnx` |

The LOSO seed reports, validator verdict, and fresh seed sidecars all carry
ADR-0661 `run_provenance`. The LOSO reports record the corpus and training
arguments that produced each fold summary. The export sidecars record the
corpus, PROMOTE verdict, argv, per-seed ONNX/sidecar targets, and optional
registry target, so a production seed refresh can be audited without
reconstructing the handoff shell session.

## Current Use

Use the kit when handing the retrain workflow to another operator or
when running the full loop locally:

```bash
bash tools/ensemble-training-kit/run-full-pipeline.sh --ref-dir /path/to/netflix/ref
bash tools/ensemble-training-kit/make-distribution-tarball.sh /tmp/vmaf-ensemble-kit.tar.gz
bash tools/ensemble-training-kit/tests/test_platform_detect.sh
```

For manual retry / debug runs, the numbered scripts are stable entry
points:

```bash
bash tools/ensemble-training-kit/01-prereqs.sh
REF_DIR=/path/to/netflix/ref bash tools/ensemble-training-kit/02-generate-corpus.sh
bash tools/ensemble-training-kit/03-train-loso.sh
bash tools/ensemble-training-kit/04-validate.sh
bash tools/ensemble-training-kit/05-bundle-results.sh
```

The lower-level scripts remain available for maintainers who are
developing the trainer itself, but operator handoff should use the kit
so the argv order, platform detection, corpus extraction, and result
bundle shape are all exercised together.

## See also

- [`ensemble-v2-real-corpus-retrain-runbook.md`](ensemble-v2-real-corpus-retrain-runbook.md)
  — the operator runbook the kit packages.
- [`docs/ai/models/fr_regressor_v2.md`](models/fr_regressor_v2.md) —
  model card for the regressor the kit retrains.
- [ADR-0324](../adr/0324-ensemble-training-kit.md) — design.

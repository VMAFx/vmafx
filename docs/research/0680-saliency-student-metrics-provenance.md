# Research 0680: Saliency-student metrics provenance

## Question

Which saliency-student training artifacts still wrote durable metrics JSON
without the shared ADR-0661 `run_provenance` block?

## Findings

- `train_saliency_student.py --metrics-out` wrote validation IoU, history,
  ONNX checksum, parity diff, and training hyperparameters, but not the DUTS
  root or command line that produced the metrics file.
- `train_saliency_student_v2.py --metrics-out` copied the same metrics shape,
  so v1/v2 comparisons could drift from the shared provenance contract even
  though both model cards cite those metrics as production evidence.
- The ONNX sidecar is produced by the trainer itself, not a later exporter, so
  the metrics file is the most useful durable artifact for replaying a DUTS
  training run after local shell history is gone.

## Decision

Add `run_provenance` to the optional saliency-student metrics payload for both
v1 and v2. Record the DUTS-TR root as the named input, the ONNX and metrics
paths as named outputs, the parsed training arguments, and the original argv.

## Commands

```bash
rg -n "metrics_out|run_provenance" ai/scripts/train_saliency_student*.py
.venv/bin/python -m pytest ai/tests/test_saliency_student_metrics_provenance.py -q
```

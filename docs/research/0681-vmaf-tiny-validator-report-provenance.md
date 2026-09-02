# Research 0681: tiny-VMAF validator report provenance

## Question

Which tiny-VMAF validation CLIs still produced promotion evidence only as
terminal output, without the shared ADR-0661 `run_provenance` block?

## Findings

- `validate_vmaf_tiny_v2.py`, `validate_vmaf_tiny_v3.py`, and
  `validate_vmaf_tiny_v4.py` compute the PLCC/RMSE smoke gate used in their
  model cards, but previously only printed the result.
- The validators already accept the key replay inputs: ONNX path, feature
  parquet, row cap, PLCC threshold, input tensor name, and optional comparison
  model(s). Those inputs are exactly the missing identity for a durable report.
- The LOSO and multi-seed evaluators already write provenance-backed reports;
  adding the same optional report path to the smoke validators closes the
  shortest promotion-evidence path without changing existing stdout behaviour.

## Decision

Add `--out-json` to the v2/v3/v4 tiny-VMAF validators. Keep stdout and exit
codes unchanged. When a report path is provided, write PLCC, RMSE, sample
predictions/truth, gate status, optional comparison-model deltas, and
`run_provenance` for the ONNX/parquet inputs and JSON output.

## Commands

```bash
rg -n "validate_vmaf_tiny_v[234]|out-json|run_provenance" ai/scripts docs/ai
.venv/bin/python -m pytest ai/tests/test_vmaf_tiny_validator_reports.py -q
```

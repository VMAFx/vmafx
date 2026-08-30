# Research 0683: AI validation report provenance

## Question

Which `validate_*.py` scripts still produced release or model-card evidence only
as terminal output after the tiny-VMAF validator report pass?

## Findings

- `validate_model_registry.py` is the CI/release trust-root validator for
  `model/tiny/registry.json`, but it only printed pass/fail and errors.
- `validate_saliency_student.py` checks the saliency-student ONNX allowlist,
  PyTorch-vs-ORT parity, and registry consistency, but it also only printed
  results.
- `validate_ensemble_seeds.py` and the vmaf_tiny v2/v3/v4 validators already
  write ADR-0661 reports, so the remaining validate-family gap is registry plus
  saliency validation.

## Decision

Add optional `--out-json` reports to `validate_model_registry.py` and
`validate_saliency_student.py`. Keep stdout and exit codes unchanged. The
reports record check verdicts/errors and `run_provenance` for registry/schema or
ONNX inputs plus the report target.

## Commands

```bash
rg -n "validate_model_registry|validate_saliency_student|out-json" ai/scripts docs/ai
.venv/bin/python -m pytest ai/tests/test_validation_report_provenance.py -q
```

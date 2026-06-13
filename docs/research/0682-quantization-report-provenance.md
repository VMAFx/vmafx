# Research 0682: quantisation report provenance

## Question

Which quantisation entry points still produced int8 promotion evidence only as
terminal output, without ADR-0661 `run_provenance`?

## Findings

- `ptq_dynamic.py` and `ptq_static.py` write the int8 ONNX artifact but only
  print the fp32/int8 byte sizes and selected quantisation settings.
- `qat_train.py` writes the fp32 bridge and final int8 ONNX artifact, but its
  CLI evidence stopped at stdout.
- `measure_quant_drop.py` is the CI gate for fp32-vs-int8 PLCC drop and was the
  only remaining quantisation gate without an optional JSON report.
- The per-EP quantisation harness already emits provenance-backed JSON, so the
  missing gap is the shipped-model producer/gate path, not the exploratory
  hardware investigation path.

## Decision

Add optional durable reports to the quantisation producer/gate scripts:
`--report-out` for dynamic PTQ, static PTQ, and QAT, and `--out-json` for the
quant-drop gate. Keep existing stdout and exit codes unchanged. The reports
record size/gate statistics plus `run_provenance` for model, calibration/config,
registry, and report paths.

## Commands

```bash
rg -n "ptq_dynamic|ptq_static|qat_train|measure_quant_drop" ai/scripts docs/ai
.venv/bin/python -m pytest ai/tests/test_ptq_scripts.py ai/tests/test_qat_smoke.py -q
```

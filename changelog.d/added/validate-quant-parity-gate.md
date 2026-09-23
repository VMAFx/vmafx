**Add clip-level fp32-vs-int8 quantization parity gate `validate_quant_parity.py`** (Research-2029, #1242)

- Added `ai/scripts/validate_quant_parity.py` and test suite `ai/tests/test_validate_quant_parity.py`
  to gate fp32-vs-int8 drift on real feature clip datasets (`testdata/scores_cpu_576.json`,
  `ai/testdata/bisect/features.parquet`) against Research-2029 §6 acceptance thresholds:
  mean absolute delta $\le 0.10$ VMAF points, maximum single-frame absolute delta $\le 0.50$ VMAF
  points, and PLCC $\ge 0.990$.
- Emits structured JSON reports with ADR-0661 run provenance (`--out-json`) and supports `--fp32` /
  `--int8` overrides. Shipped dynamic PTQ models fail strict defaults as expected until QAT
  retraining under Epic #1246.

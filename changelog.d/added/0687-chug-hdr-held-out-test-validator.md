- **`ai/scripts/validate_chug_hdr_mos_head.py`**: held-out test-partition
  validator for the CHUG HDR MOS head. Loads a CHUG MOS head ONNX, filters
  CHUG feature JSONL shards to `split == "test"` rows (552 rows, never used
  during training or model selection), runs ONNX inference via `onnxruntime`,
  computes PLCC / SROCC / RMSE vs the `mos` column, and emits a JSON report
  + Markdown report + `ai-run-provenance-v1` run-manifest sidecar.
  Production gate: PLCC ≥ 0.85 AND SROCC ≥ 0.82 AND RMSE ≤ 0.45 (exit 0
  pass / exit 2 fail). ADR-0687.
- **`ai/tests/test_validate_chug_hdr_mos_head.py`**: 19 unit tests covering
  schema column derivation, split filtering, PLCC/SROCC/RMSE computation,
  gate logic, and CLI argument parsing.
- **`docs/ai/chug-hdr-held-out-validator.md`**: user-facing documentation
  for the validator (usage, arguments, outputs, current result).
- **`docs/research/0715-chug-hdr-held-out-test-validation-2026-05-27.md`**:
  research digest for the first held-out test run. Result:
  PLCC 0.8468 / SROCC 0.8188 / RMSE 0.2639 — gate FAIL (PLCC and SROCC
  each miss by < 0.004); model remains `Status: Proposed`.

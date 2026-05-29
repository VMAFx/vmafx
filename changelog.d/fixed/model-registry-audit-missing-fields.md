- `model/tiny/registry.json`: added missing `license`, `license_url`, and
  `sigstore_bundle` fields to the `fr_regressor_v1` entry; the registry
  schema (ADR-0211) requires these fields for all non-smoke entries.
- `model/tiny/registry.json`: registered `smoke_multi_output_v0` and
  `smoke_v0_symbolic_batch` as `smoke: true` entries; both ONNX files were
  referenced by `test_vmaf_use_tiny_model.c` but absent from the registry,
  causing the validator to report 24 registered models while 26 files exist.
- `docs/ai/model-registry.md`: updated the CI-only smoke-fixtures table to
  list the two newly registered entries.
- `ai/src/aiutils/jsonl_utils.py`: resolved an unresolved merge conflict that
  was breaking `validate_model_registry.py` imports (dropped the incoming-side
  duplicate `from pathlib import Path` / `from typing import Iterator`; kept
  HEAD's `_sanitize_nonfinite` + `dumps_jsonl_row` additions).

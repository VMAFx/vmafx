<!-- markdownlint-disable MD060 -->
# Research-0724 — AI strict JSON stdout helper

## Problem

The previous strict-JSON sweeps covered AI manifest files, tool reports, and
tiny-model registry updates. A smaller gap remained in AI scripts that print
report JSON to stdout: they still used local `json.dumps()` calls, so a
non-finite diagnostic could appear as non-standard `NaN` / `Infinity` on the
operator-facing stream even when the matching `--out-json` path was strict.

## Decision

Add `aiutils.run_manifest.dumps_manifest_json()` as the string-returning peer of
`write_manifest_json()`. Route report-style stdout paths through it in:

- `ai/scripts/eval_probabilistic_proxy.py`
- `ai/scripts/eval_saliency_per_mb.py`
- `ai/scripts/hardware_caps_loader.py`

The helper is also reused by `vmaf_train.registry.dumps_registry_json()` so the
registry wrapper and manifest writer share one strict JSON boundary.

## Alternatives considered

| Option | Benefit | Cost | Outcome |
|---|---|---|---|
| Leave stdout paths alone | Smallest diff | File output and stdout disagree on non-finite values | Rejected |
| Call `write_manifest_json()` into a temporary file and print it | Avoids a new helper | Wasteful, more error paths, awkward for stdout-only CLIs | Rejected |
| Add `dumps_manifest_json()` beside `write_manifest_json()` | One shared serializer for file and stdout surfaces | Small public helper to document/test | Chosen |
| Rewrite corpus JSONL rows through the helper | Broad consistency | Would change row-level missing-value semantics during active K150K work | Rejected |

## Validation

```bash
.venv/bin/python -m pytest ai/tests/test_run_manifest.py ai/tests/test_registry_json.py ai/tests/test_eval_saliency_per_mb.py ai/tests/test_legacy_eval_report_run_provenance.py ai/tests/test_hardware_caps.py -q
.venv/bin/ruff check ai/src/aiutils/run_manifest.py ai/src/aiutils/__init__.py ai/src/vmaf_train/registry.py ai/scripts/eval_probabilistic_proxy.py ai/scripts/eval_saliency_per_mb.py ai/scripts/hardware_caps_loader.py ai/tests/test_run_manifest.py ai/tests/test_registry_json.py ai/tests/test_eval_saliency_per_mb.py ai/tests/test_legacy_eval_report_run_provenance.py ai/tests/test_hardware_caps.py
.venv/bin/black --check ai/src/aiutils/run_manifest.py ai/src/aiutils/__init__.py ai/src/vmaf_train/registry.py ai/scripts/eval_probabilistic_proxy.py ai/scripts/eval_saliency_per_mb.py ai/scripts/hardware_caps_loader.py ai/tests/test_run_manifest.py ai/tests/test_registry_json.py ai/tests/test_eval_saliency_per_mb.py ai/tests/test_legacy_eval_report_run_provenance.py ai/tests/test_hardware_caps.py
```

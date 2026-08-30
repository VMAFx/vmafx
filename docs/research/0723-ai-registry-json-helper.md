<!-- markdownlint-disable MD060 -->
# Research-0723 — AI registry strict JSON helper

## Problem

Model train/export scripts update `model/tiny/registry.json` from several
places. Those update paths used local `json.dumps()` calls. If a future
diagnostic metric or provenance value becomes non-finite, Python can write
`NaN` / `Infinity`, which are not valid RFC-8259 JSON and can break release
validation or downstream registry consumers.

## Decision

Add `vmaf_train.registry.write_registry_json()` and route model-registry update
paths through it. The helper keeps the current registry shape, sorted keys, and
trailing newline while normalizing non-finite floats to JSON `null` and using
`allow_nan=False`.

This does not change corpus JSONL writers. Training rows still have their own
schema and missing-feature semantics; this PR is scoped to the durable tiny-model
registry metadata file.

## Alternatives considered

| Option | Benefit | Cost | Outcome |
|---|---|---|---|
| Leave per-script `json.dumps()` | Smallest diff | Non-standard JSON can still leak into registry metadata | Rejected |
| Reuse `aiutils.write_manifest_json()` directly in every script | Strict output | Keeps the registry policy implicit and repeats call-site intent | Rejected |
| Add a registry-named wrapper around strict JSON | Central registry contract, smaller future edits | One thin helper to maintain | Chosen |
| Change corpus JSONL writers at the same time | Bigger sweep | Risks changing training-row missing-value semantics | Rejected |

## Validation

```bash
.venv/bin/python -m pytest ai/tests/test_registry_json.py ai/tests/test_fr_regressor_run_provenance.py -q
.venv/bin/ruff check ai/src/vmaf_train/registry.py ai/tests/test_registry_json.py ai/scripts/train_fr_regressor.py ai/scripts/train_fr_regressor_v2.py ai/scripts/train_fr_regressor_v2_ensemble.py ai/scripts/train_fr_regressor_v3.py ai/scripts/export_tiny_models.py ai/scripts/export_transnet_v2.py ai/scripts/export_transnet_v2_placeholder.py ai/scripts/export_fastdvdnet_pre.py ai/scripts/export_fastdvdnet_pre_placeholder.py
.venv/bin/black --check ai/src/vmaf_train/registry.py ai/tests/test_registry_json.py ai/scripts/train_fr_regressor.py ai/scripts/train_fr_regressor_v2.py ai/scripts/train_fr_regressor_v2_ensemble.py ai/scripts/train_fr_regressor_v3.py ai/scripts/export_tiny_models.py ai/scripts/export_transnet_v2.py ai/scripts/export_transnet_v2_placeholder.py ai/scripts/export_fastdvdnet_pre.py ai/scripts/export_fastdvdnet_pre_placeholder.py
```

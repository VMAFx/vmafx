<!-- markdownlint-disable MD060 -->
# Research-0721 — AI run manifest strict JSON

## Question

Can the shared AI run-manifest writer prevent non-standard JSON tokens across
training/export/report sidecars without changing script-local report assembly?

## Findings

- `aiutils.run_manifest.write_manifest_json()` is already the central write
  boundary for many AI manifests, model sidecars, report JSON files, and
  validation verdicts.
- Script-local reports can legitimately compute `NaN` / `Infinity` while
  exploring weak model fits or failed gates, but those tokens are not valid
  RFC-8259 JSON when written by Python's default `json.dumps()`.
- The helper already normalizes paths, mappings, tuples, and lists; adding
  non-finite float normalization at this layer fixes all callers that route
  through the shared writer without touching each training script.

## Decision

Normalize non-finite floats to `null` in `normalise_manifest_value()` and make
`write_manifest_json()` serialize the normalized payload with
`allow_nan=False`. This keeps manifests strict while preserving script-local
data structures before the write boundary.

## Alternatives considered

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| Patch each caller | Local control per schema | Repeats the same bug fix across dozens of scripts | Rejected |
| Keep raw `json.dumps()` | No behavior change | Allows invalid JSON tokens in durable evidence files | Rejected |
| Normalize in shared writer | One central contract for reports and manifests | Callers that bypass the helper remain future backlog | Chosen |

## Validation

```bash
.venv/bin/python -m pytest ai/tests/test_run_manifest.py -q
.venv/bin/ruff check ai/src/aiutils/run_manifest.py ai/tests/test_run_manifest.py
.venv/bin/black --check ai/src/aiutils/run_manifest.py ai/tests/test_run_manifest.py
```

<!-- markdownlint-disable MD060 -->
# Research-0722 — tool report strict JSON

## Problem

Two fork-local user-facing tools still used raw `json.dumps()` on durable
operator reports:

- `vmaf-roi-score` wrote its final result payload directly.
- `tools/external-bench/compare.py --out-json` wrote aggregate rows directly.

Python's default encoder accepts non-finite floats and writes `NaN` /
`Infinity`, which are not valid RFC-8259 JSON. That makes the files fragile for
strict downstream parsers, especially when a wrapper has no valid rows and the
external-bench aggregate means are intentionally `NaN` in memory.

## Decision

Keep each tool's public schema unchanged and make only the write boundary
strict:

- `vmaf-roi-score` now returns exit code 65 when the underlying `vmaf` score is
  non-finite, so it does not write an invalid result file.
- `external-bench --out-json` renders aggregate rows with `allow_nan=False` and
  converts non-finite aggregate means to JSON `null`.

## Alternatives considered

| Option | Benefit | Cost | Outcome |
|---|---|---|---|
| Leave raw `json.dumps()` | Smallest diff | Strict JSON parsers still reject user-facing files | Rejected |
| Reject empty external-bench aggregates | No `null` values | A missing optional external binary would make smoke reports fail instead of preserving a clear missing-data row | Rejected |
| Convert non-finite aggregate means to `null` only at JSON output | Portable JSON and unchanged in-memory/table semantics | JSON and table represent missing data differently (`null` vs `nan`) | Chosen |
| Bump `vmaf-roi-score` schema | Explicit version signal | No field shape changed; a schema bump would create needless churn | Rejected |

## Validation

```bash
.venv/bin/python -m pytest tools/vmaf-roi-score/tests tools/external-bench/tests -q
.venv/bin/ruff check tools/vmaf-roi-score/src/vmafroiscore/cli.py tools/vmaf-roi-score/tests/test_combine.py tools/external-bench/compare.py tools/external-bench/tests/test_compare.py
.venv/bin/black --check tools/vmaf-roi-score/src/vmafroiscore/cli.py tools/vmaf-roi-score/tests/test_combine.py tools/external-bench/compare.py tools/external-bench/tests/test_compare.py
```

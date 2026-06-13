<!-- markdownlint-disable MD060 -->
# Research-0718 — vmaf-tune sidecar strict JSON persistence

## Question

After the report and CLI JSON helpers landed, which `vmaf-tune` JSON writers
still exposed user-visible or operator-visible artifacts to Python's default
`NaN` / `Infinity` tokens?

## Findings

- `SidecarModel.save()` still called `json.dump()` directly for
  `${XDG_CACHE_HOME:-~/.cache}/vmaf-tune/sidecar/<predictor>/<codec>/state.json`.
  That file is local-only, but operators inspect it and the CLI reports derive
  from the same state, so it should follow the same strict JSON policy.
- Report rendering already used `dumps_strict()`, but through a local wrapper in
  `report.py`; that extra wrapper made the strict JSON path look report-local
  instead of package-local.
- Corpus JSONL and cache keys intentionally remain separate: corpus rows carry
  training-schema missing-feature semantics, and cache keys must not be changed
  as part of a report/sidecar portability cleanup.

## Decision

Add `vmaftune.jsonio.write_json_strict()` for atomic strict JSON writes, route
local sidecar state through it, and reject a strict-nullified non-finite state as
cold-start on reload. This keeps corrupt local corrections from being replayed
while preserving strict JSON parseability.

## Alternatives considered

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| Leave sidecar persistence on raw `json.dump()` | Smallest diff | Operator-visible state can contain non-standard JSON tokens | Rejected |
| Coerce non-finite loads back to `0.0` | Keeps a state file loadable | Silently hides corrupt weights/history | Rejected |
| Null non-finite values on write and cold-start on reload | Strict JSON plus fail-safe recovery | Discards the local sidecar after pathological state | Chosen |

## Validation

```bash
.venv/bin/python -m pytest tools/vmaf-tune/tests/test_sidecar.py tools/vmaf-tune/tests/test_report.py -q
.venv/bin/ruff check tools/vmaf-tune/src/vmaftune/jsonio.py tools/vmaf-tune/src/vmaftune/report.py tools/vmaf-tune/src/vmaftune/sidecar.py tools/vmaf-tune/tests/test_sidecar.py
.venv/bin/black --check tools/vmaf-tune/src/vmaftune/jsonio.py tools/vmaf-tune/src/vmaftune/report.py tools/vmaf-tune/src/vmaftune/sidecar.py tools/vmaf-tune/tests/test_sidecar.py
```

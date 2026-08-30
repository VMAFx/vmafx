# Research-0715 — vmaf-tune strict JSON helper

## Context

The follow-up `vmaf-tune` helper sweep found duplicate JSON portability logic
in the report renderer and compare emitter. Both paths coerce `NaN` /
`Infinity` to `null` and then call `json.dumps(..., allow_nan=False)` so
report/profile consumers can use strict JSON parsers.

## Findings

- `report.py` and `compare.py` had equivalent recursive non-finite-float
  coercion helpers with package-local comments explaining the same BBB report
  bug class.
- The strict JSON rule is not report-template-specific; compare JSON, sweep
  JSON, and embedded report appendices all need the same RFC-8259 behaviour.
- A small `vmaftune.jsonio` helper keeps the behaviour package-local and avoids
  importing AI-specific manifest/provenance concepts into tune.

## Alternatives considered

- **Keep both private helpers.** Rejected because the next tune JSON artifact
  would likely grow a third copy.
- **Expose the helper from `report.py`.** Rejected because compare JSON should
  not depend on the report renderer.
- **Adopt a repo-wide JSON helper immediately.** Rejected for this slice; tune
  has known non-finite report semantics, while other packages need their own
  schema audits before sharing a cross-repo utility.

## Smoke

```bash
.venv/bin/python -m pytest tools/vmaf-tune/tests/test_compare.py tools/vmaf-tune/tests/test_report.py -q
```

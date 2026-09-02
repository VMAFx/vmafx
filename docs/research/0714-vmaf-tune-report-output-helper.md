# Research-0714 — vmaf-tune report output helper

## Context

The cross-tool helper sweep found the first `vmaf-tune` report seam in
`cli.py`: both inline compare profile rendering and the standalone `report`
subcommand manually selected HTML / Markdown suffixes, created output
directories, and wrote rendered artifacts. The standalone path also owned the
status aggregation logic that dashboards consume after rendering.

## Findings

- `ReportData` already centralises the structured report payload and encoder
  profile schema, so output writing belongs beside the renderer rather than in
  every CLI caller.
- The top-level status JSON is part of the report surface, but the logic was
  only testable through the full CLI. Moving it into `vmaftune.report` keeps the
  behaviour identical while making unavailable-codec vs real-failure semantics
  directly testable.
- No schema or text changes are needed for this slice. The helper preserves the
  current `html`, `markdown`, and `both` suffix rules and the current status
  fields.

## Alternatives considered

- **Leave the duplication in `cli.py`.** Rejected because the next report/profile
  writers would copy the same suffix and status handling again.
- **Create a cross-package helper shared with AI.** Rejected for this slice
  because `vmaf-tune` report artifacts have package-specific HTML/Markdown
  rendering and status semantics; premature cross-package naming would couple
  unrelated schemas.
- **Change the report JSON schema now.** Rejected because this is a refactor
  slice. User-facing profile improvements should land as explicit feature PRs.

## Smoke

```bash
.venv/bin/python -m pytest tools/vmaf-tune/tests/test_report.py tools/vmaf-tune/tests/test_compare_rate_quality_sweep.py::test_cli_compare_profile_report_both_writes_html_and_markdown -q
```

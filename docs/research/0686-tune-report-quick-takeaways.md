# Research 0686: vmaf-tune report quick takeaways

## Question

What report-side addition best helps non-expert users understand a
`vmaf-tune report` without weakening the existing machine-readable profile?

## Findings

- Existing profile cards already contain charts, tables, codec badges, failure
  status, and the raw `encoder_profile` payload.
- The missing piece is a run-specific plain-language summary. Static docs cannot
  say which codec won a given sweep, which rows failed, or whether the per-shot
  CRF spread is meaningful.
- The renderer has all required structured data in `ReportData`; no encoder or
  VMAF invocation is needed.

## Decision

Render a Quick takeaways section in Markdown and HTML. Derive it from
`ReportData`: Pareto frontier for sweep reports, best successful row for
single-target reports, failed/unavailable row count, ladder span, and per-shot
CRF range.

## Commands

```bash
rg -n "Quick takeaways|_quick_takeaways" tools/vmaf-tune/src/vmaftune/report.py tools/vmaf-tune/tests/test_report.py docs/usage/vmaf-tune.md
.venv/bin/python -m pytest tools/vmaf-tune/tests/test_report.py -q
```

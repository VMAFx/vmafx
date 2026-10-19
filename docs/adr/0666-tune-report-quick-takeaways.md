<!-- markdownlint-disable MD013 MD025 MD033 MD060 -->

# ADR-0666: vmaf-tune report quick takeaways

- **Status**: Accepted (status update 2026-10-06 below)
- **Date**: 2026-05-21
- **Deciders**: Lusoris maintainers
- **Tags**: vmaf-tune, reports, ux, encoder-profile

## Context

`vmaf-tune report` already renders rate-quality charts, codec identity chips,
tables, and an embedded encoder profile. That is enough for expert readers, but
the project goal is for profile cards to guide human operators quickly: a reader
should see the recommendation, coverage gaps, ladder span, and per-shot spread
before interpreting the detailed chart.

## Decision

Every Markdown and HTML profile-card report will render a **Quick takeaways**
section immediately after source metadata. The section is generated from the
same structured `ReportData` as the tables: Pareto winners for sweep reports,
best single-target codec rows for legacy compare reports, failed/unavailable
row counts, ladder bitrate/resolution span, and per-shot CRF range.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Leave interpretation to tables and charts | No rendering change | Non-expert users must infer the answer from multiple sections | Does not meet the human-facing report goal |
| Add prose only to documentation | No report-schema churn | The guidance is separated from the actual run and cannot mention the real winner or failure count | Operators need run-specific guidance in the artifact they share |
| Generate takeaways from raw JSON in the browser | Keeps Markdown simpler | HTML-only and harder to test; Markdown/PDF exports lose the summary | The renderer is already pure Python data-to-string and can cover both formats |

## Consequences

- **Positive**: HTML and Markdown reports surface the recommendation and risk
  context before charts, improving scanability for operators and reviewers.
- **Negative**: Existing snapshot-style tests need to account for an extra
  section in rendered output.
- **Neutral / follow-ups**: Future report-profile work can add structured
  machine-readable summary keys, but this PR keeps the encoder-profile schema
  stable.

## References

- [Research-0686](../research/0686-tune-report-quick-takeaways.md)
- [ADR-0643](0643-vmaf-tune-encoder-profile-contract.md)
- Source: `req` — "they need to be helpful to the human users to quickly understand what the data really says -> means it shouldn't be only for experts"

### Status update 2026-10-05: Accepted

Per [ADR-0106](0106-adr-maintenance-rule.md) the body above is frozen; this
note records why the status line changed from Proposed, as found by the
2026-10-05 ADR status sweep. The Quick takeaways section in Markdown and HTML
reports shipped in PR 1489 and is rendered by
tools/vmaf-tune/src/vmaftune/report.py. Evidence on `master`: PR #1489,
`fe2169230`.

Verification command:

```bash
git grep -n 'Quick takeaways' origin/master -- tools/vmaf-tune/src/vmaftune/report.py
```

## Status update 2026-10-06: The unfilled template block was removed

Until this date the file began with an unfilled template block from the ADR
number allocator (stub comments, the title `<fill in title>`, `<fill in>`
sections), followed by this record. The block carried no decision and made the
first heading, the Deciders line and the Tags line unreadable by tools; it was
removed. The record itself is unchanged.

The body above is unchanged.

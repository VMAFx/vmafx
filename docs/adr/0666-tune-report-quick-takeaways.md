# ADR-0666: vmaf-tune report quick takeaways

- **Status**: Accepted
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
best single-target codec rows for legacy compare reports, failed/unavailable row
counts, ladder bitrate/resolution span, and per-shot CRF range.

## Alternatives considered

| Option                                          | Pros                   | Cons                                                                                              | Why not chosen                                                                |
| ----------------------------------------------- | ---------------------- | ------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------- |
| Leave interpretation to tables and charts       | No rendering change    | Non-expert users must infer the answer from multiple sections                                     | Does not meet the human-facing report goal                                    |
| Add prose only to documentation                 | No report-schema churn | The guidance is separated from the actual run and cannot mention the real winner or failure count | Operators need run-specific guidance in the artifact they share               |
| Generate takeaways from raw JSON in the browser | Keeps Markdown simpler | HTML-only and harder to test; Markdown/PDF exports lose the summary                               | The renderer is already pure Python data-to-string and can cover both formats |

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
- Source: `req` — "they need to be helpful to the human users to quickly
  understand what the data really says -> means it shouldn't be only for
  experts"

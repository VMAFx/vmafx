<!-- markdownlint-disable MD060 -->
# ADR-0659: Modernization audit false-positive filter

- **Status**: Accepted
- **Date**: 2026-05-20
- **Deciders**: Lusoris, Codex
- **Tags**: developer-tools, backlog, docs, fork-local

## Context

ADR-0658 added a read-only modernization audit so agents can find real backlog
after a long merge train. The first live run ranked historical text such as
"replaces the `NotImplementedError` scaffold" and Python exception handlers as
high-severity implementation gaps. That polluted `.workingdir2` triage with
already-closed QAT and Phase-B rows and made the operator revalidate stale
findings before reaching real code work.

## Decision

We will filter historical closeout wording, non-debt Python exception plumbing,
and documented `-ENOSYS` disabled-build contracts before creating marker
findings. A live `raise NotImplementedError(...)` remains actionable. A
docstring that says an old scaffold was replaced, an `except NotImplementedError`
handler, a custom exception class inheriting from `NotImplementedError`, or a
documented optional-build `-ENOSYS` fallback is not reported as a modernization
gap.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the raw text scan | Finds every marker with no heuristics | Continues to rank closed historical prose above real gaps | Rejected — the audit is a queue-shaping tool, so precision matters more than literal marker recall |
| Maintain a file-level suppressions list | Precise per false-positive row | Adds another state file to keep fresh and hides future real gaps in suppressed files | Rejected — the current false positives share simple local line context |
| Filter historical / exception-handler / disabled-build context | Keeps live `raise NotImplementedError` and bare `return -ENOSYS` rows while removing stale prose and documented contracts | A future unusual marker may need another local context rule | Chosen — small, testable, and keeps the report useful without manual suppressions |

## Consequences

- **Positive**: modernization reports stop promoting QAT / Phase-B historical
  closeout text and documented DNN / workflow `-ENOSYS` contracts as fresh work.
- **Negative**: the marker scan is now heuristic instead of a pure regex pass.
- **Neutral / follow-ups**: future false positives should add a minimal context
  test rather than broad file suppressions.

## References

- [ADR-0658](0658-project-modernization-audit.md) — Project modernization audit.
- Research: [Research-0659](../research/0659-modernization-audit-false-positive-filter.md)
- Source: `req` ("well go on i guess we have enough backlog...")

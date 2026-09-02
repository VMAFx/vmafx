<!-- markdownlint-disable MD013 MD060 -->
# ADR-0810: ADR-0108 Six-Deliverables Compliance Audit (2026-05-29) + D3 Gap Fixes

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `docs`, `agents`, `process`

## Context

ADR-0108 mandates that every fork-local PR ships six deliverables: (1) research
digest, (2) decision matrix in the ADR's `## Alternatives considered`, (3) an
`AGENTS.md` invariant note, (4) a reproducer/smoke-test command in the PR body,
(5) a `changelog.d/` fragment, and (6) a `docs/rebase-notes.md` entry.

A compliance audit run on 2026-05-29 covering the five most recently merged PRs
(#1573, #1568, #1582, #1583, #1571, all merged 2026-05-28) found a 93 % pass rate
(28/30 deliverables present). The sole recurring gap was deliverable D3: two PRs
(#1583 and #1571) did not register their rebase-sensitive invariants in `AGENTS.md`
even though the relevant notes existed in `docs/rebase-notes.md`.

This ADR documents both the audit findings and the D3 remediation applied in the
same PR.

## Decision

We accept the audit findings as a point-in-time baseline. The two D3 gaps are fixed
immediately:

1. Root `AGENTS.md` §13 receives a new invariant row for the `libvmaf/ → core/`
   rename (PR #1571, ADR-0700) and a new row for the vmafx-server `[http]`
   optional dependency group (PR #1583, ADR-0701).
2. `mcp-server/AGENTS.md` receives an HTTP transport dispatch-order invariant note.
3. Two stale `libvmaf/AGENTS.md` path references in root §13 are corrected to
   `core/AGENTS.md`.

A process fix is also recorded: the PR template checklist item for D3 should
require the author to choose one of two branches — a filled invariant note or an
explicit opt-out sentinel with a reason. Prose-only opt-outs in the PR body are
insufficient for automated auditing.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Fix D3 gaps retroactively on the merged commits | Source of truth is the commit | Rewriting merged history violates branch-protection | Rejected — apply fixes as a follow-up commit on a new branch, which is the standard pattern |
| Require re-opening and re-merging the two non-compliant PRs | Formally correct | Extremely disruptive; both PRs are stable and the gap is documentation-only | Rejected — proportional response is a follow-up fix PR |
| Defer D3 fixes to the next PR touching those files | Zero extra PR | Gap persists in `AGENTS.md` for an unknown duration, visible to all agents | Rejected — D3 gap is actionable today at near-zero cost |

## Consequences

- Root `AGENTS.md` §13 and `mcp-server/AGENTS.md` are now accurate for the
  two 2026-05-28 deliverables.
- The audit research digest (`docs/research/adr-0108-compliance-audit-2026-05-29.md`)
  provides a snapshot baseline for future automated compliance scripts.
- The recurring D3 failure pattern is named: "rebase-notes substitute without AGENTS.md
  index row" — future audits can grep for this pattern.

## References

- req: "Sample 5 recently-merged PRs from the last 24h and verify the ADR-0108 six deliverables are present + cited correctly."
- ADR-0108: `docs/adr/0108-deep-dive-deliverables-rule.md`
- ADR-0700: `docs/adr/0700-vmafx-repo-layout.md` (PR #1571 — D3 gap)
- ADR-0701: `docs/adr/0701-vmafx-cloud-native-redesign.md` (PR #1583 — D3 gap)
- Audit report: `docs/research/adr-0108-compliance-audit-2026-05-29.md`

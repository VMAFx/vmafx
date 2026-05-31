# ADR-0106: Every non-trivial decision gets its own ADR file before the commit

- **Status**: Accepted
- **Supersedes**: [ADR-0028](0028-adr-maintenance-rule.md)
- **Date**: 2026-04-18
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: docs, planning, agents

## Context

Earlier sessions made decisions (directory moves, CI gate changes,
dependency swaps), committed them, and ended with the rationale only
recoverable from commit messages — which typically summarise *what*
changed but omit *what alternatives were considered*. The user
flagged that the ADR log had drifted out of date relative to actual
decisions. The risk: a future contributor cannot tell which decisions
were weighed against alternatives versus which were defaults.

The first version of this rule (ADR-0028, since superseded by this
ADR) codified the practice against a single mega-table at
`docs/adr/decisions-log.md`. On 2026-04-17 the fork migrated to the
[Nygard one-file-per-decision format](https://cognitect.com/blog/2011/11/15/documenting-architecture-decisions)
that the golusoris framework template uses; the rule itself is
unchanged but the on-disk shape is now one
`docs/adr/NNNN-*.md` per decision indexed by [README.md](README.md).

This ADR re-states ADR-0028's decision in neutral English so the body
no longer carries colloquial verbatim quotes. The decision itself is
unchanged; only the prose register is.

## Decision

Every non-trivial architectural, policy, or scope decision made in any
coding session against this fork ships as its own file
`docs/adr/NNNN-kebab-case.md` following [0000-template.md](0000-template.md),
and gets a row added to [README.md](README.md) in the same PR as the
commit that implements it. *Non-trivial* means anything another
engineer could reasonably have chosen differently: directory moves,
base-image policy, CI-gate semantics, test-selection rules, dependency
additions, coding-standards changes. Bug fixes and implementation
details do not need an ADR. Each ADR cites its source in
`## References` — `req` (direct user quote) or `Q<round>.<q>` (popup
answer). The `## Context` and `## Alternatives considered` sections
capture *why* and *what else was on the table*, not *what* (the code
shows what). Enforced by session-start discipline: re-read
`docs/adr/README.md` and skim any ADRs referenced in the current
conversation before committing; add missing files for decisions
inherited from conversation history.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Rely on commit messages alone | Zero new surface | Summaries omit alternatives; unauditable; hard to diff | Rejected per user direction |
| ADR per code commit (rigid) | Maximum coverage | Noise from bug fixes and cosmetic changes | Non-trivial filter is sufficient |
| Single mega-table `decisions-log.md` (prior shape) | Low-ceremony | Merge conflicts on every entry; no individual file history; diverges from Nygard / MADR / golusoris convention | Superseded by one-file-per-decision on 2026-04-17 |
| One ADR file per decision (chosen) | Matches Nygard / MADR + golusoris template; per-file git history; no merge conflicts on parallel PRs; linkable from per-package AGENTS.md | Requires an index file (`README.md`) to stay in sync | Rationale matches |

The rule preserves "we chose X over Y because Z" in an auditable,
linkable per-decision file. Migration from the mega-table to per-file
ADRs was executed in one sweep on 2026-04-17 by renumbering D1–D42 to
ADR-0001–ADR-0042 in chronological commit order.

## Consequences

- **Positive**: design history is auditable; session continuity
  survives context loss; parallel PRs do not conflict on the ADR log;
  each ADR has its own git history; per-package `AGENTS.md` files can
  link directly to the ADRs that govern their subtree.
- **Negative**: each session must re-read the index and classify
  decisions as trivial or not; writing a new ADR costs roughly five
  minutes per decision.
- **Neutral / follow-ups**: [CLAUDE.md §12 rules 8 and 9](../../CLAUDE.md)
  and [AGENTS.md §12 rule 8](../../AGENTS.md) codify the flow. The
  legacy `decisions-log.md` was deleted as part of the migration PR;
  its contents are preserved one-ADR-per-file in this directory.
  ADR-0028 stays in the tree, marked `Superseded by ADR-0106`.

## References

- Source: `req` (user: "oh and somehow you forget to maintain the adr's")
- Related source: `req` (user, 2026-04-17:
  "somehow the adr format is not like in golusoris? lol shoudlnt this
  be a file per adr?")
- Supersedes: [ADR-0028](0028-adr-maintenance-rule.md)
- Format: [Michael Nygard — Documenting Architecture Decisions](https://cognitect.com/blog/2011/11/15/documenting-architecture-decisions)
- Template: [joelparkerhenderson/architecture-decision-record](https://github.com/joelparkerhenderson/architecture-decision-record)
- Related ADRs: every ADR in this directory follows this rule.

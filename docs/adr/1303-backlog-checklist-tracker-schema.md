<!-- markdownlint-disable MD013 MD060 -->
# ADR-1303: Give checklist backlog items explicit stable IDs

- **Status**: Accepted
- **Date**: 2026-09-23
- **Deciders**: Lusoris
- **Tags**: `agents`, `tooling`, `workspace`, `docs`

## Context

[ADR-0355](0355-symphony-agent-dispatch-infra.md) made the local backlog the
pre-dispatch authority and implemented `BacklogTracker` against its then-current
pipe table. The canonical ledger later became a shorter Markdown checklist and
dropped every `T-*` row ID. The parser consequently returned zero items while
the file held 22 checkboxes. Commit `6475fa9ea` already made an unknown ID block
dispatch, so the live failure is no longer fail-open, but no tracked backlog
item can pass the first eligibility check.

The checklist has titles, list positions, section headings, and GitHub issue
references. None is a safe implicit identifier: titles and ordering are
editorial, headings describe phase rather than identity, and several entries
have zero or multiple issue references. Choosing any of them in the parser
would silently make a different tracker authoritative. The evidence and
alternatives are recorded in
[the schema migration digest](../research/1303-backlog-tracker-schema-migration-2026-09-23.md).

## Decision

Every tracked checklist item carries a stable backtick ID immediately after
its checkbox:

```markdown
2. [ ] `T-RC1-MASTER-GREEN` **Master green and the queue drained**
7. [ ] `T-RC1-BENCH-TUNE` **[BLOCKED]** **Bench and tune**
1. [x] `T-RC1-RELEASE-PIPELINE` **Release pipeline correct and idle**
```

The checkbox is the closed-state authority: `[x]` is `DONE`, and `[ ]` is
`OPEN`. An unchecked title may begin with exactly `**[BLOCKED]**`,
`**[DEFERRED]**`, or `**[IN_FLIGHT]**`; these override `OPEN` without
duplicating a closed state. Indented continuation lines belong to the item and
participate in PR-reference extraction.

`BacklogTracker` keeps read compatibility with the retired pipe-table schema.
It never synthesises an ID. An unmarked checklist item, duplicate stable ID,
unknown explicit status, any status marker on a checked item, a closed-state
marker (`DONE`, `CLOSED`, or `REMOVED`) on an unchecked item, or an existing
ledger with zero tracked items raises `BacklogFormatError`. The eligibility CLI
converts that exception into a named blocking verdict with no traceback. The
module remains read-only; human operators edit the git-ignored ledger.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Explicit stable IDs on checklist items** (chosen) | Human-visible, survives reorder and rewording, keeps the local ledger authoritative, and can coexist with legacy rows. | Requires a one-time manual migration and an ID on each new checkbox. | Only option that restores useful lookup without guessing identity. |
| Derive IDs from list positions or title slugs | No ledger annotation. | Reordering or editorial rewording changes identity and can dispatch duplicate work. | Identity would be unstable by construction. |
| Use the first GitHub issue reference as the ID | Reuses a hosted identifier. | Some items have no issue, some name several, and the issue tracker is not the same authority as local planning. | Silently changes which system owns backlog identity. |
| Retire `--backlog-id` and require free-form `--task-tag` | Smallest code change. | Skips both backlog-state and merged-PR checks, discarding ADR-0355's purpose. | Safe but no longer useful as reconciliation. |
| Restore the historical pipe table | Existing parser needs no change. | Re-expands a deliberately condensed local ledger and makes human editing harder. | Parser should follow the chosen editorial format, not force its retired one back. |

## Consequences

- **Positive**: the canonical checklist parses into 22 typed items rather than
  zero, and phase gates such as benchmark/retrain are machine-visible as
  `BLOCKED`.
- **Positive**: schema drift fails closed at the parser boundary instead of
  degrading to an empty result or an unknown-ID diagnosis.
- **Positive**: legacy private ledgers remain readable during migration.
- **Negative**: every new checkbox needs one stable ID and an explicit status
  marker when it is not simply open or done.
- **Neutral**: `.workingdir/BACKLOG.md` remains ignored and manually edited;
  the committed fixture proves the schema while the operator smoke proves the
  current local ledger.

## References

- [ADR-0355](0355-symphony-agent-dispatch-infra.md) — original tracker and
  pre-dispatch decision.
- [Research: backlog tracker schema migration](../research/1303-backlog-tracker-schema-migration-2026-09-23.md).
- Source: `req` — "everything that isnt related to any retraining? because we
  fix everything until we cant find anything anymore for now and then we will
  tune for speed".
- Source: `req` — "all bugs.md's in this local repo should of course be fully
  fixed".

<!-- markdownlint-disable MD013 MD060 -->
# Backlog tracker schema migration evidence — 2026-09-23

## Question

How should `scripts/lib/backlog_tracker.py` regain useful authority after the
git-ignored canonical backlog moved from a `T-*` pipe table to Markdown
checklists without silently inventing identity from prose?

## Measured starting state

- `BacklogTracker().path` resolves the intended
  `/home/kilian/dev/vmafx/vmafx/.workingdir/BACKLOG.md` from an agent worktree.
- The canonical file contains 22 Markdown checkboxes and zero legacy table
  rows. Before this change, `len(BacklogTracker().all())` is `0`.
- The dangerous historical behavior in the local BUGS note is already stale:
  commit `6475fa9ea` changed a missing item from notice/pass to error/fail.
  `--backlog-id T-RC1-MASTER-GREEN --skip-gh-search --skip-active-scan`
  therefore exits 1 as unknown on master rather than dispatching.
- The remaining break is functional: because every real item is unknown, an
  operator must use `--task-tag`, which intentionally skips backlog-state and
  merged-PR reconciliation.

## Candidate identities checked

| Candidate | Evidence | Verdict |
|---|---|---|
| List number | Exists only in the eight-item Road to 1.0.0 section; bullets elsewhere have no number, and editorial reorder changes it. | Reject. |
| Normalised title | Every item has one, but rewording a title changes its machine identity and can evade active/merged-work searches. | Reject. |
| GitHub issue reference | Several rows have none; others name a parent plus several tasks or PRs. The local backlog and GitHub are deliberately distinct authorities. | Reject. |
| Section plus ordinal | Covers every row but combines two editorial values and is still unstable on move/reorder. | Reject. |
| Explicit stable ID | Adds one visible token, survives every editorial change above, and preserves the `BacklogItem.id` / CLI contract. | Choose. |

## Chosen schema and failure behavior

The exact item prefix is a list marker, a checkbox, and a stable ID in a
backtick span. A checked box is `DONE`; an unchecked box is `OPEN` unless its title begins with one of
the three explicit non-closed markers `**[BLOCKED]**`, `**[DEFERRED]**`, or
`**[IN_FLIGHT]**`. Continuation lines are part of the item so a wrapped
`PR #N` remains discoverable.

Fail-closed cases are as important as happy-path parsing:

1. Any checklist checkbox without the backtick ID is a schema error.
2. Duplicate IDs are errors rather than "first one wins".
3. A checked row cannot also declare an open-class status marker.
4. An existing backlog with no tracked item is an error, not an empty queue.
5. The CLI catches the schema exception and reports a blocking, named verdict
   rather than a Python traceback.

Legacy pipe rows stay readable so archived/local ledgers do not need an atomic
rewrite. The current local ledger was migrated manually and now parses 22
items: 9 closed, 8 open, 3 blocked, and 2 deferred. The phase-order smoke proves
`T-RC1-MASTER-GREEN` is eligible, while `T-RC1-BENCH-TUNE` and
`T-RC1-MODEL-RETRAIN` are blocked before dispatch.

## Verification contract

- Synthetic current checklist: ID, title, continuation, PR ref, all four live
  status classes.
- Legacy table compatibility.
- Missing ID, duplicate ID, and prose-only existing file all raise
  `BacklogFormatError`.
- CLI malformed-schema case exits 1, names `malformed BACKLOG.md`, and emits no
  traceback.
- Operator smoke against the git-ignored canonical ledger checks real phase
  IDs without freezing its mutable row count into CI.

## Decision

Adopt the explicit checklist ID schema in [ADR-1303](../adr/1303-backlog-checklist-tracker-schema.md).

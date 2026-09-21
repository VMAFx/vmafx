<!-- markdownlint-disable MD029 -->
# ADR-0165: Tracked `docs/state.md` for bug-status hygiene (T7-1)

- **Status**: Accepted
- **Date**: 2026-04-25
- **Deciders**: Lusoris
- **Tags**: process, state-hygiene, claude-rule, fork-local

## Context

[Issue #20](https://github.com/VMAFx/vmafx/issues/20) flagged a recurring
failure mode: Claude Code agents re-investigate already-closed bugs at the
start of each session, because no in-flight bug-status record gets updated
during the session that closed them. The closing comment on Issue #20
made the fix concrete:

> What's NOT yet done and matches the exact ask in this issue: a
> `STATE.md` file + commit-as-you-close discipline for *bug status*
> (vs. architectural decisions, which D28 covers). Leaving open until
> STATE.md exists and has a rule wired in CLAUDE.md §12.

The fork already has two state-shaped surfaces:

- `docs/adr/` — **decisions** (one file per non-trivial architectural
  / policy choice). [ADR-0028](0028-adr-maintenance-rule.md) makes
  these immutable once Accepted.
- `.workingdir2/{OPEN,BACKLOG,PLAN}.md` — **planning dossier**.
  Gitignored; intentionally local-only so that mid-flight task notes
  don't leak into the repo. Not a candidate for tracked bug status.

What's missing is a **tracked, in-tree** surface that says "these bugs
are open, these are closed, here is the PR / ADR that closed each one,
and here are the bugs we ruled out as not-affecting-the-fork." Without
it, future sessions reach for `git log` (slow, lossy, not query-able)
or guess from `.workingdir2/` (gitignored, can be stale on a fresh
clone).

## Decision

Ship a single tracked file [`docs/state.md`](../state.md) with three
sections:

1. **Open bugs** — known issues actively under investigation or queued
   for fix. Each row carries: bug ID (Netflix#N or fork-local), summary,
   reproducer status, owning ADR or PR if any, target.
2. **Recently closed** — bugs closed in the last ~3 months. Each row
   carries: bug ID, summary, closing PR + ADR, verification method.
   Older entries roll off into the git log naturally.
3. **Confirmed not-affected / explicitly deferred** — the negative
   results that protect future sessions from re-investigating dead
   ends. Includes Netflix issues that don't apply to fork's code paths
   and deliberate defers (e.g. Netflix#955 ADR-0155 deferred-pending-
   upstream).

Wire the update discipline as **CLAUDE.md §12 rule 13** (next free
slot in the hard-rules block):

> 13. **Every** PR that closes a bug, opens a bug, or rules a Netflix
>     upstream report not-affecting-the-fork updates `docs/state.md`
>     in the **same PR**. The update lands a row in the appropriate
>     section (Open / Recently closed / Confirmed not-affected) and
>     cross-links the ADR, PR, and Netflix issue (if any). State drift
>     compounds across sessions; the rule trades a 30-second edit for
>     hours of re-investigation cost when context resets.

## Alternatives considered

1. **Promote `.workingdir2/OPEN.md` to tracked**. Rejected: the
   planning-dossier convention is load-bearing — `.workingdir2/`
   accumulates work-in-progress notes that are deliberately not
   shipped. Inverting that convention to track one file would either
   blur the boundary (some `.workingdir2/` files tracked, others
   not — confusing) or require migrating multiple files all at once.
   The narrow ask is bug-status, not "all planning notes."

2. **Just add the rule to CLAUDE.md without a tracked file**. Rejected:
   without a target file, the rule has no anchor — every PR description
   is a different shape and bugs scroll off page-1 of `git log`
   quickly. The whole point of Issue #20 was to give future sessions
   a single place to look.

3. **Use GitHub Issues as the bug-status surface**. Rejected for
   fork-local provenance: GitHub Issues are great for inbound triage
   but lossy when a Netflix-side PR resolves a fork-local issue
   indirectly, or when the fork *deliberately defers* (e.g. ADR-0155).
   In-tree Markdown gives the audit trail traceable cross-links to
   ADRs / PRs / commits without depending on an external service that
   could rate-limit or change UI.

## Consequences

**Positive:**

- One canonical place for "what bugs are still real?"
- Cross-linkable: ADRs and PR descriptions can link to specific
  rows in `docs/state.md` for grounding.
- Low write cost: most rows are 1–2 sentences plus links.
- Survives session resets and fresh clones.

**Negative:**

- Yet another rule to remember. Mitigated by binding it to PR-merge
  events (the same moment Conventional Commit + ADR rules already
  fire). The PR template ([.github/PULL_REQUEST_TEMPLATE.md](../../.github/PULL_REQUEST_TEMPLATE.md))
  carries a checkbox so reviewers verify it.
- Risk of drift between `docs/state.md`, `.workingdir2/BACKLOG.md`,
  and the closed-issues view on GitHub. Convention: `docs/state.md`
  is authoritative for **bug status** only; backlog items
  (T-numbers) and ADR decisions stay in their respective surfaces.
- Format will need to evolve. Section headings stay stable; row
  shape can be refactored without an ADR amendment.

## References

- Issue #20 — original ask (closed COMPLETED 2026-04-19; this ADR
  is the concrete artifact that re-opens the audit trail).
- [ADR-0028](0028-adr-maintenance-rule.md) — ADR-row-before-commit
  rule (the decision-log half of state hygiene).
- BACKLOG T7-1 — backlog row.
- `req` — user direction 2026-04-25: "well then update the state files
  thats bullshit as well" → "well then lets go" (popup choice:
  tracked `docs/state.md`).

### Status update 2026-05-09: comprehensive verify-every-row audit landed

A comprehensive verify-every-row audit of `docs/state.md` ran on
2026-05-09 (per [Research-0090](../research/0090-state-md-row-audit-2026-05-09.md)).
The trigger was five honest-NO-OP findings earlier in the session
(VK Step A / T6-1 / T6-2a-A / HP-5 v1 / T7-5 / T6-9) caused by
stale rows pointing at deferred work that had actually shipped
weeks ago. The `state-md-touch` CI gate (#479) prevents *future*
drift but does not catch *historical* drift.

Per Research-0090 §4 aggregate (49 sub-rows audited):

- **Open**: 3 rows. V=3 / S=0 / G=3 (all genuinely open per
  ADR-Accepted decisions: ADR-0264 / ADR-0269 / ADR-0273).
- **Deferred (dataset)**: 1 active row. V=1 / S=0 / G=1
  (PR #466 still OPEN).
- **Recently closed**: 41 sub-rows. V=33 / S(backfill)=8 / G=0.
- **Confirmed not-affected**: 3 rows. V=3 / S=0 / G=0.
- **Deferred (external-trigger)**: 1 row. V=1 / S=0 / G=1
  (Netflix#1494 still OPEN per `gh pr view --repo Netflix/vmaf`).

The 8 STALE rows were all "this PR" -> post-merge backfill — the
row was added in the closing PR's branch using "this PR" as a
placeholder, the merge happened, but the placeholder was never
rewritten to the merged numeric PR. This is a *new* drift mode
the `state-md-touch` gate does not catch (it only checks state.md
was *touched*, not that it *names a real merged PR*). Rows
updated to the merged numeric refs in the same PR as this status
update: PRs #511, #470, #424, #420, #419, #414, #337, #155, #173.

No row was found to be incorrectly Open; per the user's
`feedback_no_test_weakening` rule, none were closed-to-clean.

Verification commands cited inline per row in Research-0090
(`gh pr view <N> --json state,mergedAt,title --jq …` for PR
claims; `gh pr view <N> --repo Netflix/vmaf` for upstream-watch
rows; `grep` / `find` for file / symbol claims). All VERIFIED
rows got an `_(verified 2026-05-09)_` annotation in their
rightmost column so a future spot-check is distinguishable from
a pre-audit row.

### Status update 2026-09-21: the gate now reads section against status

The "Decision" above partitions the ledger into sections and the
"Update protocol" in [`docs/state.md`](../state.md) says a PR that
fixes a bug **moves** its row. Neither was machine-checked.
`scripts/ci/check-state-md-rows.sh` enforced only the *duplicate*
consequence of a missed move — the same id under two sections, or the
same row twice — which is what a "keep both sides" rebase produces.

A missed move need not produce a duplicate at all. If the row is
appended to, or simply left under, `## Open bugs` while its own
rightmost cell already reads `closed` or `fixed`, there is exactly one
copy of it and every existing check passes. The bug then reads as open
for every future session, which is the outcome this ADR exists to
prevent. Measured on `master` at `fb06193b3`: 24 of the 62 rows under
`## Open bugs` declared `closed` (6) or `fixed` (18) in their own
status cell, and the gate reported the file clean. The row for
`T-STATE-MD-ROW-GATE-BLIND-2026-09-16` was one of the 24.

The gate now pairs each row's section heading with the status token in
its status cell and requires them to agree — `closed` / `fixed` /
`resolved` / `done` only under `## Recently closed`, `open` only under
`## Open bugs`. The status cell is the column a table header calls
`Status`, or the last non-empty cell when no header names one, and the
token read is the word that *opens* that cell: `| fixed (PR #1425) |`
is the same misfiled row as `| fixed |` and is judged the same way. The
row shapes that lead with no status token — a verification date, a
branch name, prose — make no section claim and are not guessed at.

What the gate does *not* do is see every misfiled row. It reads one
cell per row, so a status the extraction does not recognise — buried
mid-cell, in a column that is neither the last nor headed `Status`, or
spelled outside the vocabulary — is passed over in silence and the file
still reports clean. Of the two ways the check can be silently
disabled, it fails closed on one: if some row claims a status belonging
to a section whose heading has been renamed or removed, the gate errors
rather than passing over rows that have become ungated. The other — an
unrecognised status cell — is uncovered, and is the likelier of the
two, since it takes a single row edit rather than a heading rename. The
check is a floor on this class of drift, not a proof of its absence.

This is an enforcement change, not a new decision — the move
discipline was already decided here and in the file's own update
protocol. The 24 rows were moved into `## Recently closed` in the
section's reverse-chronological order, with a tombstone comment left
where they were; no status cell was edited to match where its row had
landed and no row was deleted.

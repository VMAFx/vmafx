---
paths:
  - scripts/ci/check-state-md-rows.sh
  - scripts/ci/tests/test-check-state-md-rows.sh
invariant: Five checks, none may narrow; status agrees with section; fix by moving row; markup closes per line.
---
<!-- markdownlint-disable MD013 MD060 -->
# check-state-md-rows.sh — the status token belongs to the section (ADR-0165)

Five independent checks, first four widened only after narrower version
reported dirty file as clean. Do not narrow any of them.

1. Duplicate bug id. Matches four id shapes (`**T-ID**`, `T-ID`, `**T7-16**`,
   `Netflix/vmaf#NNN`) and anchors on token that OPENS first cell, not
   on whole cell — most rows carry description after id.
2. Verbatim repeated row, for the ~143 prose-led rows that carry no id.
   Normalises away `_(verified YYYY-MM-DD: ...)_` before comparing.
3. Section against status. row's status cell — column table header
   calls `Status`, else last non-empty cell — must agree with level-2
   heading row sits under whenever token OPENING that cell is status
   word: `closed` / `fixed` / `resolved` / `done` only under
   `## Recently closed`, `open` only under `## Open bugs`.
4. Open row against move tombstone. comment under `## Open bugs` that says
   id "moved to Recently closed" is explicit closed-state claim; same id
   may not still have table row in that section, even when row has no
   parseable Status cell.
5. Markup that closes on its line. Every line outside fenced block pairs each
   backtick run with later run of same length (CommonMark code span)
   and closes every `[` outside code span (`markup_defect()`). Most of
   ledger is one paragraph of rows: single stray backtick re-pairs every code
   span after it; `[` it leaves outside span made GFM
   autolink-literal parser of markdown governance gate walk back to it from
   every later URL candidate. One row with lldb's frame name in single
   backticks took lint of this file from 4 s to 140-250 s, past gate's
   fixed 120 s budget (`T-STATE-MD-UNPAIRED-CODE-SPAN-LINT-TIMEOUT-2026-10-06`,
   cordanaLLM/praetor#784). backtick that belongs to text goes inside
   longer run (``` `` mod`close `` ```); `[` with no `]` goes into code span
   or is escaped (`\[`). Do not exclude `docs/state.md` from style lint
   instead.

Check 3 exists because checks 1 and 2 only see *duplicate*. resolved row
left under `## Open bugs` with no second copy is invisible to both, and reads
as open bug forever; 24 of 62 rows were in that state on 2026-09-21.

Check 4 covers remaining no-Status shape. 2026-09-08 PTQ bookkeeping
change claimed row had moved and left its tombstone immediately below
unchanged Open row; first three checks all reported clean. Preserve
fixture that rejects that exact contradiction. tombstone plus row under
`## Recently closed` is valid moved state and must continue to pass.

Invariants for check 3:

- judged cell is `Status` column when table header names one and
  last non-empty cell otherwise, and token read is word that OPENS it.
  Requiring whole cell to BE status token caught `| fixed |` and skipped
  `| fixed (PR #1425) |` — same misfiled row, one parenthetical later —
  along with 20 other live rows. Reading leading word leaves shapes
  that make no claim unjudged: verification date or parenthetical leads
  with no word at all, branch name leads with `fix` rather than `fixed`
  (`fix/...`, `ci/...`), prose leads outside vocabulary. Widening
  vocabulary to guess at those fabricates failures — eight live rows end in
  branch name. Cells are split on unescaped `|` only: `\|` inside inline
  code span is escaped pipe renderer shows, not boundary, and
  splitting on it shifts every later cell by one.
- Check 3 is floor on this class of drift, not proof of its absence. It
  reads one cell per row: status it does not recognise — buried mid-cell,
  in column that is neither last nor headed `Status`, or spelled outside
  vocabulary — is passed over in silence and file still reports clean.
- Of its two silent-disable paths, it fails closed on ONE. If row claims
  status whose owning section heading is absent, gate errors rather than
  passing over rows that have become ungated, so renaming `## Open bugs` or
  `## Recently closed` breaks build on purpose. other path — status
  cell extraction does not recognise — is uncovered, and is likelier
  of two, since it needs one row edit rather than heading rename. This
  file and ADR-0165 both asserted that check fails closed on its *one*
  silent-disable path; that was false when written and claim is corrected
  here rather than left standing.
- fix for hit is always to MOVE row. Rewriting status to match
  where row landed is failure, dressed up as repair.

Header rows are excluded same way for all three checks: `|---|---|`
separator retracts record for line immediately above it, and only when
that line is itself table row — `prev` and `prevline` both reset on
non-table line. separator whose header was lost to dropped rebase hunk
otherwise retracts status of last data row above blank line, which
silently unjudges real row.

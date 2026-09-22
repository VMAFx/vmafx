- **The `docs/state.md` row-hygiene gate called 24 closed bugs open.**
  ADR-0165 says a fixed bug's row *moves* from "Open bugs" to "Recently
  closed"; `scripts/ci/check-state-md-rows.sh` enforced that only by
  hunting duplicates — the same id under two sections, or the same row
  twice. A closed bug reads as open forever without any duplicate being
  involved: the row sits under `## Open bugs` while its own rightmost
  cell already says `closed` or `fixed`, because the PR appended its row
  to the section it happened to be reading instead of moving it, or a
  rebase dropped the move hunk and kept the status edit. 24 of the 62
  rows under `## Open bugs` were in that state — among them the row for
  this gate's own bug, `T-STATE-MD-ROW-GATE-BLIND-2026-09-16` — and the
  gate reported the file clean, because nothing about any of them is a
  duplicate. The gate now reads the section heading each row sits under
  together with the status token in its status cell and requires the two
  to agree: `closed` / `fixed` / `resolved` / `done` only under
  "Recently closed", `open` only under "Open bugs". The status cell is
  the column a table header calls `Status`, or the last non-empty cell
  when no header names one, and the token read is the word that *opens*
  that cell — a cell reading `fixed (PR #1425)` is the same misfiled row
  as one reading `fixed` and is judged the same way. Rows that lead with
  no status token — a verification date, a branch name, prose — make no
  status claim and are left alone rather than guessed at, so the check
  adds no fabricated failures.
  The check is a floor on this class of drift, not a proof of its
  absence: it reads one cell per row, so a status it does not recognise
  is passed over in silence. Of the two ways it can be silently
  disabled it fails closed on one — if a row claims a status belonging
  to a section whose heading has been renamed or deleted, the gate
  errors instead of passing over rows that have quietly become ungated
  — while the other, a status cell the extraction does not recognise,
  stays uncovered and is the likelier of the two.
  All 24 rows were **moved** into "Recently closed", in the section's
  reverse-chronological order and with a tombstone comment left where
  they were. No status cell was rewritten to match where its row had
  landed, and no row was deleted. Nine new gate tests cover the misfiled
  row, its mirror (an `open` row under "Recently closed"), a status
  carrying a trailing parenthetical, a `Status` column that is not the
  last one, an escaped `\|` that would otherwise shift the judged
  column, and a separator row whose header line is missing; the
  renamed-heading no-op gets a fixture that isolates it plus a control
  that passes, and a further case pins the rows that carry no status
  token as passing. The suite runs green under gawk, mawk and busybox
  awk.

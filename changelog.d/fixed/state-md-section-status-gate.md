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
  together with the status token in its last cell and requires the two
  to agree: `closed` / `fixed` / `resolved` / `done` only under
  "Recently closed", `open` only under "Open bugs". Rows whose last cell
  is a verification date, a branch name or prose make no status claim
  and are left alone rather than guessed at, so the check adds no
  fabricated failures. It fails closed on the one way it could be
  silently disabled: if a row claims a status belonging to a section
  whose heading has been renamed or deleted, the gate errors instead of
  passing over rows that have quietly become ungated.
  All 24 rows were **moved** into "Recently closed", in the section's
  reverse-chronological order and with a tombstone comment left where
  they were. No status cell was rewritten to match where its row had
  landed, and no row was deleted. Three new gate tests cover the
  misfiled row, its mirror (an `open` row under "Recently closed") and
  the renamed-heading no-op; a fourth pins the rows that carry no status
  token as passing.

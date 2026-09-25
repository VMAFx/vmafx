- **Reject contradictory `docs/state.md` move bookkeeping.** The row-hygiene
  gate now fails when an id remains as a table row under Open bugs while an
  adjacent tombstone says it moved to Recently closed. The stale static-PTQ
  row is moved to Recently closed, matching the QDQ fix and regression coverage
  already delivered by PR #1306.

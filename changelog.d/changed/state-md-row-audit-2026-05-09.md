- **`docs/state.md` comprehensive verify-every-row audit (2026-05-09).** Every
  row in the Open / Deferred / Recently closed / Confirmed not-affected /
  Deferred-external sections was independently re-verified against current
  master, the relevant ADR statuses, and the GitHub PR / Issue API. Eight
  "Recently closed" rows whose closer was recorded as the placeholder
  "this PR" were backfilled with the merged numeric PR refs (PRs #511,
  #470, #424, #420, #419, #414, #337, #155, #173). Every VERIFIED row
  carries a new `_(verified 2026-05-09)_` annotation in its rightmost
  column. No Open / Deferred row was found to be falsely-open (per the
  fork's no-test-weakening rule). See
  [Research-0090](../docs/research/0090-state-md-row-audit-2026-05-09.md)
  and [ADR-0165 Status update 2026-05-09](../docs/adr/0165-state-md-bug-tracking.md#status-update-2026-05-09-comprehensive-verify-every-row-audit-landed).

- **docs**: drift sweep for `docs/adr/README.md` vs.
  `docs/adr/_index_fragments/`. Reconciles ~37 pre-existing rows that PR
  #476's append-only backfill flagged as out-of-scope (running
  `concat-adr-index.sh --write` blindly would have either dropped 38 README
  rows whose fragments did not exist or kept 6 duplicate rows from
  fragment-side updates that bypassed regen). Resolution: backfilled 36
  fragment files from the existing README row content (the row content
  already reflects each ADR's accepted state per ADR-0028), renamed 2
  fragments whose ADR file had been renumbered by the 2026-05-02 dedup
  sweep (`0270-saliency-…` → `0286-saliency-…`,
  `0287-vmaf-tune-saliency-aware` → `0293-vmaf-tune-saliency-aware`),
  fixed 2 fragment bodies whose `[ADR-NNNN](slug.md)` links referenced
  pre-renumber slugs (`0263-ossf-scorecard-policy`,
  `0255-fastdvdnet-pre-real-weights`), removed one stale `_order.txt`
  entry that pointed at a missing fragment
  (`0297-vmaf-tune-sample-clip` superseded by `0301-…`), and appended the
  37 new slugs to `_order.txt`. README diff: −48 / +36 lines (duplicates
  collapsed, missing rows restored). New section in
  `docs/development/release.md` documents the regenerate-vs-append policy
  mirroring PR #480's CHANGELOG drift-sweep precedent.

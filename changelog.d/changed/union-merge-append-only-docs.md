- Git: `docs/rebase-notes.md`, `docs/adr/_index_fragments/_order.txt`,
  `docs/adr/README.md` and `CHANGELOG.md` carry `merge=union`. Every merge to
  master appends to them, so every other open PR conflicted on them and needed a
  hand rebase — 32 such conflicts across three batch rebases on 2026-09-05, all
  resolved identically by keeping both sides, which is what the union driver
  does automatically. `docs/state.md` is deliberately excluded: its rows *move*
  between sections, so keeping both sides duplicates a row (the defect gated by
  `scripts/ci/check-state-md-rows.sh`).

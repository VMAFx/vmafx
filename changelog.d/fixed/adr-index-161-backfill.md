- **Indexed 160 previously-unlisted ADRs (CLAUDE §12 r8).** `docs/adr/` held 878
  ADR files but only ~718 had an `_index_fragments/` row, so 160 Accepted ADRs —
  including RC-band ADR-1101..1108 — were absent from the rendered
  `docs/adr/README.md` index. Generated a fragment (`| ID | Title | Status |
  Tags |`, matching the authoritative 4-column header) for each from its
  heading/Status/Tags, inserted it into `_order.txt` at its numeric position, and
  re-rendered. `scripts/docs/concat-adr-index.sh --check` exits 0. NOTE: the index
  carries a pre-existing 4-vs-5-column inconsistency (some older rows added a Date
  cell the header never declared); the backfill follows the header's 4-column
  form — normalising the ~71 5-column rows is a separate follow-up.

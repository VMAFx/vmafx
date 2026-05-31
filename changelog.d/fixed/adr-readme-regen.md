- `docs/adr/README.md` regenerated from `docs/adr/_index_fragments/`
  to clear a 84-row drift between the rendered index and the per-ADR
  fragment tree. The README had 481 unique IDs with 194 duplicate rows
  (677 total) plus three rows for ADRs (0805 / 0806 / 0811) whose files
  were never committed to master; the fragment tree was missing 35
  fragments for ADRs landed since the last regen and contained eight
  malformed rows (four-column entries with a date in the tag slot, plus
  six fragments whose link target referenced the pre-renumber slug).
  This PR scaffolds the missing fragments from each ADR's `Title` /
  `Status` / `Tags`, drops three orphan fragments left over from prior
  renumber sweeps, deduplicates `_index_fragments/_order.txt`, removes
  the three stale README-only entries, and re-runs
  `scripts/docs/concat-adr-index.sh --write`. Final state: 630 ADR
  files (excluding `0000-template.md`), 630 fragments, 630
  `_order.txt` entries, 630 unique-ID README rows — all four sources
  aligned, `--check` clean. Unblocks PRs that have been opting out of
  README regeneration to avoid the 84-row churn.

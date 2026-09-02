- **docs**: backfill the `docs/adr/_index_fragments/` manifest for eight
  ADRs whose bodies had landed but whose `README.md` index rows were
  missing — discovered by the 27-ADR sweep audit (PR #468). Adds six
  new fragment files (ADR-0235 codec-aware fr_regressor, ADR-0236 DISTS
  extractor, ADR-0238 Vulkan picture preallocation, ADR-0239 GPU
  picture-pool dedup, ADR-0251 Vulkan async pending-fence, ADR-0279
  fr_regressor_v2 probabilistic), appends them to `_order.txt`, and
  surfaces the eight rows (the six new + ADR-0276 fast-path +
  ADR-0315 vendor-neutral VVC encode strategy, both of which had
  fragments + order entries already but no README row) in
  `docs/adr/README.md`. Pure index-maintenance — no ADR-body edits and
  no behaviour change.

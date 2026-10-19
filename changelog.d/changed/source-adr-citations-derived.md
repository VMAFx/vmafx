- **Citing an ADR in source no longer edits a shared registry
  ([ADR-2200](docs/adr/2200-source-adr-citations-derived.md)).** The source
  ADR-citation gate derives each binding from the tree; `scripts/ci/source-adr-citations.json`
  keeps only the retired and fixture records and `--write` is gone.

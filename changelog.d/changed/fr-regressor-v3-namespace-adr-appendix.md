- **ADR-0302 status appendix — namespace collision resolved
  (ADR-0349).** Append-only status update on
  [ADR-0302](../docs/adr/0302-encoder-vocab-v3-schema-expansion.md)
  per [ADR-0028](../docs/adr/0028-adr-maintenance-rule.md)
  (Accepted-ADR immutability) records that the `fr_regressor_v3`
  registry row stays authoritative for the vocab-16 retrain and
  that the future canonical-6 + `encoder_internal` + shot-boundary
  + `hwcap` feature-set work claims the reserved name
  `fr_regressor_v3plus_features` per
  [ADR-0349](../docs/adr/0349-fr-regressor-v3-namespace.md).
  No code change in ADR-0302 itself.
- **`ai/AGENTS.md` gains a `## fr_regressor_* namespace map`
  section** that enumerates the claimed names
  (`_v1`, `_v2`, `_v2_ensemble_v1_seed{0..4}`, `_v3`) and
  reserves `_v3plus_features`. Future agents working on the
  `fr_regressor` lineage cite this map before claiming a new id.

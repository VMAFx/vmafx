- **fr_regressor_v2 ensemble seeds flip smoke → production
  ([ADR-0320](docs/adr/0320-fr-regressor-v2-ensemble-seed-flip.md)).**
  The five `fr_regressor_v2_ensemble_v1_seed{0..4}` rows in
  `model/tiny/registry.json` flip from `smoke: true` to `smoke: false`
  on the verdict from the
  [ADR-0319](docs/adr/0319-ensemble-loso-trainer-real-impl.md) harness
  end-to-end run against the locally-generated Phase A canonical-6
  corpus (5,640 rows × 9 Netflix sources × `h264_nvenc` × 4 CQs):
  mean per-seed LOSO PLCC = **0.9973** (gate `≥ 0.95` ✓) and per-seed
  spread = **9.5e-4** (gate `≤ 0.005` ✓), per-seed PLCC range
  `[0.9969, 0.9978]` with no failing seeds. Verdict file committed at
  `model/tiny/fr_regressor_v2_ensemble_v1_seed_flip_PROMOTE.json` as
  the immutable audit trail. Honours the
  [ADR-0303](docs/adr/0303-fr-regressor-v2-ensemble-prod-flip.md)
  two-part gate contract on the
  [ADR-0309](docs/adr/0309-fr-regressor-v2-ensemble-real-corpus-retrain.md)
  harness output.

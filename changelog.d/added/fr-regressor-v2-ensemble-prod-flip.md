- **`fr_regressor_v2` ensemble production-flip trainer + CI gate scaffold
  (ADR-0303, builds on ADR-0291 deterministic prod flip + ADR-0279
  probabilistic head + PR #372 ensemble scaffold).** Adds
  `ai/scripts/train_fr_regressor_v2_ensemble_loso.py` — a 9-fold
  leave-one-source-out trainer over the Netflix Public Dataset for
  the five `fr_regressor_v2_ensemble_v1_seed{0..4}` registry members
  (`smoke: true` today). Each invocation emits one
  `loso_seed{N}.json` artefact per seed with per-fold PLCC / SROCC /
  RMSE so the production-flip CI gate can decide which seeds clear
  the ship threshold. The gate script
  `scripts/ci/ensemble_prod_gate.py` reads the five JSONs and passes
  iff `mean_i(PLCC_i) ≥ 0.95` **and**
  `max_i(PLCC_i) - min_i(PLCC_i) ≤ 0.005` — the variance bound is
  what protects the predictive-distribution semantics that the
  in-flight `vmaf-tune --quality-confidence` flag (ADR-0237 consumer)
  relies on; without it, the ensemble mean could mask a pathological
  one-seed-wins-four-seeds-tie configuration. The scaffold ships the
  scripts only — no registry rows flip in this PR; the actual
  `smoke: true → false` flip is a follow-up gated on a real-corpus
  LOSO run. Trainer body returns `NotImplementedError` when the real
  Phase A corpus
  (`runs/phase_a/full_grid/per_frame_canonical6.jsonl`) is missing
  so smoke-only invocations stay safe; argparse + module imports
  parse cleanly without the corpus present. CI workflow wiring of
  the gate is intentionally deferred to the flip PR (no real
  `loso_seed{N}.json` artefacts exist yet to gate on master). Docs:
  [`docs/adr/0303-fr-regressor-v2-ensemble-prod-flip.md`](../../docs/adr/0303-fr-regressor-v2-ensemble-prod-flip.md),
  [`docs/research/0075-fr-regressor-v2-ensemble-prod-flip.md`](../../docs/research/0075-fr-regressor-v2-ensemble-prod-flip.md),
  [`ai/AGENTS.md`](../../ai/AGENTS.md) "Ensemble registry invariant".

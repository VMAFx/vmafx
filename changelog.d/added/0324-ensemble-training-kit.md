- **Ensemble training kit — portable Phase-A + LOSO retrain bundle
  ([ADR-0324](../docs/adr/0324-ensemble-training-kit.md)).**
  Adds `tools/ensemble-training-kit/` with a one-command orchestrator
  (`run-full-pipeline.sh`) that chains prereqs → Phase-A canonical-6
  corpus generation → 5-seed × 9-fold LOSO retrain → ADR-0303 verdict
  emission → bundling. Five numbered step scripts (`01-prereqs.sh`
  through `05-bundle-results.sh`) are usable individually for retries.
  The kit reuses the existing in-tree pieces unchanged
  (`scripts/dev/hw_encoder_corpus.py`,
  `ai/scripts/run_ensemble_v2_real_corpus_loso.sh`,
  `ai/scripts/validate_ensemble_seeds.py`,
  `ai/scripts/export_ensemble_v2_seeds.py`,
  `scripts/ci/ensemble_prod_gate.py`) — no engine changes.
  `make-distribution-tarball.sh` produces a self-contained tar.gz
  (~ < 50 MiB) with the kit + every script it invokes + the runbook,
  so a collaborator can untar without cloning the fork. Operator-facing
  documentation lives in
  [`tools/ensemble-training-kit/README.md`](../tools/ensemble-training-kit/README.md);
  the kit's pinned Python dependency set is captured in
  `requirements-frozen.txt`. Companion to
  [ADR-0309](../docs/adr/0309-fr-regressor-v2-ensemble-real-corpus-retrain.md)'s
  runbook.

- **`fr_regressor_v2` ensemble LOSO trainer — real loader + per-fold
  training ([ADR-0319](../docs/adr/0319-ensemble-loso-trainer-real-impl.md),
  closes ADR-0303 + ADR-0309 deferrals).** Replaces the
  `NotImplementedError` stubs in
  [`ai/scripts/train_fr_regressor_v2_ensemble_loso.py`](../ai/scripts/train_fr_regressor_v2_ensemble_loso.py)
  with a real `_load_corpus` (pandas-based, validates the canonical-6
  schema emitted by `scripts/dev/hw_encoder_corpus.py`) + a real
  `_train_one_seed` (9-fold LOSO with `FRRegressor(num_codecs=14)`,
  Adam@5e-4, MSE, fold-local StandardScaler). Trainer emits
  `loso_seed{N}.json` matching the `mean_plcc` schema
  [`scripts/ci/ensemble_prod_gate.py`](../scripts/ci/ensemble_prod_gate.py)
  consumes plus per-fold PLCC/SROCC/RMSE traces per Research-0075.
  Wrapper [`ai/scripts/run_ensemble_v2_real_corpus_loso.sh`](../ai/scripts/run_ensemble_v2_real_corpus_loso.sh)
  drops the obsolete `--corpus-root` / `--output` argv and passes
  `--corpus $CORPUS_JSONL --out-dir $OUT_DIR` matching the trainer's
  interface; adds a ≥100-row prereq check.
  Runbook
  [`docs/ai/ensemble-v2-real-corpus-retrain-runbook.md`](../docs/ai/ensemble-v2-real-corpus-retrain-runbook.md)
  gains "Step 0: Generate Phase A canonical-6 corpus" with the
  `hw_encoder_corpus.py` for-loop pattern. QSV is optional — NVENC-only
  corpus still trains. Wall time: ~5 min per seed on RTX 4090 (~25 min
  for the full 5-seed run). Registry-flip stays a separate follow-up
  PR per ADR-0309's invariant. Tests under
  `ai/tests/test_train_fr_regressor_v2_ensemble_loso_{loader,train}.py`
  cover the loader contract + the gate-compatible JSON schema on
  synthetic 12-row fixtures (CPU-only, sub-second runtime).

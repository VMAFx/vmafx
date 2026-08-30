- **Per-codec predictor v2 — real-corpus LOSO trainer + ADR-0303 gate
  enforcement (Phase 2).** Companion to PR #450 (predictor training
  pipeline + 14 stub ONNX models) and PR #462 (canonical-6 corpus
  schema bump). Adds `ai/scripts/train_predictor_v2_realcorpus.py` —
  a Phase-2 trainer that consumes JSONL corpora discovered under
  `~/.workingdir2/{netflix,konvid-150k,bvi-dvc-raw}/`, runs 5-fold
  leave-one-source-out cross-validation per codec, and enforces the
  ADR-0303 production-flip gate (mean PLCC ≥ 0.95 across folds, max-
  min spread ≤ 0.005, per-fold floor ≥ 0.95). Adds
  `ai/scripts/run_predictor_v2_training.sh` orchestrator that the
  operator runs locally; it discovers corpora, invokes the trainer,
  emits a JSON report under `runs/predictor_v2_realcorpus/`, retrains
  every PASS codec on the full corpus to overwrite
  `model/predictor_<codec>.onnx`, and patches the model card with
  REAL numbers. **Codecs that FAIL the gate keep the synthetic stub
  ONNX untouched and the model card gains a
  `Status: Proposed (gate-failed: REASON)` block** — per CLAUDE.md
  §13 / `feedback_no_test_weakening`, the gate is load-bearing and
  must not be silently lowered. New tests under
  `ai/tests/test_train_predictor_v2_realcorpus.py` (22 cases) pin
  the gate threshold (0.85 PLCC reports as FAIL, never silent PASS),
  source-level LOSO partitioning (no row-level leak), corpus-
  discovery semantics (missing roots are skipped silently), and the
  JSON-report schema consumed by the orchestration shell. The PR
  ships scripts + tests only; trained-model artefacts land in a
  follow-up PR after the operator runs the trainer locally against
  a real corpus.

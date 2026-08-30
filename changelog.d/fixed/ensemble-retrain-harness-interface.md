- **`fr_regressor_v2` ensemble retrain harness — wrapper-trainer
  interface mismatch + missing Phase A pre-step doc.** PR #405
  (ADR-0309) shipped the real-corpus LOSO retrain harness without
  end-to-end testing it; two real bugs surface the moment an
  operator runs `ai/scripts/run_ensemble_v2_real_corpus_loso.sh`.
  (1) The wrapper passed `--corpus-root` and `--output` to
  `ai/scripts/train_fr_regressor_v2_ensemble_loso.py`, but the
  trainer accepts `--corpus` (a JSONL path) and `--out-dir` (the
  trainer derives `loso_seed{N}.json` itself); the trainer rejected
  the wrapper's invocation with `unrecognized arguments`. (2) The
  runbook only verified the raw YUV corpus existed, not the Phase
  A canonical-6 JSONL the trainer actually consumes; the producer
  is `scripts/dev/hw_encoder_corpus.py` (PR #392) but the runbook
  never mentioned it. Fix: wrapper now passes
  `--corpus "$CORPUS_JSONL"` (defaulting to
  `runs/phase_a/full_grid/per_frame_canonical6.jsonl`) and
  `--out-dir "$out_dir"`; the JSONL-existence check replaces the
  YUV-directory hard-fail (YUV check is informational since the
  trainer never reads YUVs). Runbook gains a step
  **0. Generate the Phase A canonical-6 corpus** with a concrete
  bash loop over 9 Netflix sources × {h264_nvenc, h264_qsv} × CQs
  ∈ {19, 25, 31, 37} → ~33,840 rows (per Research-0075). Updated
  wall-time estimate: ~3–5 h Phase A on RTX 4090 + 6–12 h LOSO on
  an 8 GB GPU class. Smoke-tested with a stub JSONL — the trainer
  no longer errors on argv parsing; it now reaches the (still
  scaffold-only) corpus loader. Trainer CLI is **not** modified
  — freezing it as authoritative keeps the companion scripts
  (`train_fr_regressor_v2_ensemble.py`,
  `eval_loso_vmaf_tiny_v3.py`,
  `scripts/ci/ensemble_prod_gate.py`) aligned. See
  [ADR-0318](../../docs/adr/0318-ensemble-retrain-harness-fix.md).

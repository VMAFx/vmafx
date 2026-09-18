# ai/sidecar — Agent invariants

Implements vmafx-node online training sidecar (ADR-0781).

## Load-bearing invariants for rebase / port agents

1. **Unix socket path constant** — `VMAFX_SIDECAR_SOCKET` / default
   `/tmp/vmafx-sidecar.sock` must stay in sync between `online_trainer.py`
   and `cmd/vmafx-node/online_feedback.go` (`feedbackSocketDefault`). Drift ->
   sidecar silently stops receiving samples.

2. **Wire protocol** — JSON envelope `{"job_id", "features", "true_score"}`
   (Python server) and `FeedbackMessage{JobID, Features, TrueScore}` (Go client)
   = matched pair. Adding field requires updating both sides atomically in
   same PR.

3. **ONNX opset** — `SGDEMATrainer.export_onnx()` uses `opset_version=17`
   (ADR-0249 constraint). Do not bump without updating op-allowlist audit
   and all downstream `onnxruntime-go` consumers.

4. **Replay buffer capacity default** — 10 000 samples = ADR-0781 design point.
   Change requires updating ADR, Helm default
   (`sidecar.trainer.replayBufferSize`), and docs table in
   `docs/ai/sidecar-online-training.md`.

5. **No NFL golden-data path** — package has no connection to Netflix
   golden-data test fixtures or `python/test/` assertion values. Changes here
   cannot affect those tests.

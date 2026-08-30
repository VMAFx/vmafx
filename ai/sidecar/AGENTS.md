# ai/sidecar — Agent invariants

This package implements the vmafx-node online training sidecar (ADR-0781).

## Load-bearing invariants for rebase / port agents

1. **Unix socket path constant** — `VMAFX_SIDECAR_SOCKET` / default
   `/tmp/vmafx-sidecar.sock` must stay in sync between `online_trainer.py`
   and `cmd/vmafx-node/online_feedback.go` (`feedbackSocketDefault`).  If
   either side drifts the sidecar silently stops receiving samples.

2. **Wire protocol** — the JSON envelope `{"job_id", "features", "true_score"}`
   (Python server) and `FeedbackMessage{JobID, Features, TrueScore}` (Go client)
   are a matched pair.  Adding a field requires updating both sides
   atomically in the same PR.

3. **ONNX opset** — `SGDEMATrainer.export_onnx()` uses `opset_version=17`
   (ADR-0249 constraint).  Do not bump without also updating the op-allowlist
   audit and all downstream `onnxruntime-go` consumers.

4. **Replay buffer capacity default** — 10 000 samples is the ADR-0781
   design point.  Changing it requires updating the ADR, the Helm default
   (`sidecar.trainer.replayBufferSize`), and the docs table in
   `docs/ai/sidecar-online-training.md`.

5. **No NFL golden-data path** — this package has no connection to the
   Netflix golden-data test fixtures or the `python/test/` assertion values.
   Changes here cannot affect those tests.

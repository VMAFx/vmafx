### Sidecar online training — SGD + EMA + replay buffer (ADR-0781)

Added a Python sidecar container (`ai/sidecar/`) that continuously fine-tunes
a tiny-AI ONNX model alongside `vmafx-node` during live encoding jobs.

- `ai/sidecar/replay_buffer.py` — thread-safe bounded ring buffer (default
  10 000 samples, FIFO eviction) for experience replay.
- `ai/sidecar/sgd_ema.py` — online SGD + EMA trainer (`beta=0.999`),
  gradient clipping, 100-step LR warmup, atomic ONNX export (opset 17).
- `ai/sidecar/online_trainer.py` — Unix-socket server
  (`/tmp/vmafx-sidecar.sock`), newline-delimited JSON protocol, mixed
  replay-buffer batching, periodic checkpoint export with SHA-256 sidecar.
- `cmd/vmafx-node/online_feedback.go` — non-blocking Go client with a
  1000-entry in-process ring buffer; fire-and-forget from the scoring path.
- `deploy/helm/vmafx/templates/sidecar-trainer.yaml` — Helm named template
  `vmafx.sidecarContainer`; gated by `.Values.sidecar.trainer.enabled`
  (default `false`); k8s 1.29+ native sidecar (`restartPolicy: Always`).

Closes Phase 4b item 4b.7 (Research-0733).

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

6. **Unix socket permissions (mode `0o660`) and server lifecycle** — The Unix domain
   socket must be created with permission mode exactly `0o660` (`rw-rw----`)
   and owned by the server process EUID/EGID. Mode `0o660` is intentional and
   mandatory to allow the same-group Go node peer running in a shared Kubernetes
   pod / `emptyDir` volume to connect and transmit feedback samples (requiring
   `stat.S_IWGRP`). Alternative permissions (`0o644` strips group write, `0o600`
   denies group access across UIDs) break IPC; mode `0o660` maintains strict
   least-privilege with zero other/world access (`0o000`). Verified with
   actual peer credentials (`SO_PEERCRED`) under Linux user namespaces with
   subordinate ID mappings. Documented inline for Semgrep alert 946 per
   ADR-1222. In SARIF, Alert 946 remains present with in-source suppression
   awaiting maintainer dismissal (False Positive / Won't Fix).
   Server lifecycle: initialize `threads`, `registry = _ConnectionRegistry()`,
   `old_sigterm`, and `old_sigint` before `bind()` to avoid masking `OSError`
   with `UnboundLocalError`. Register accepted sockets in `registry` via
   `register_if_active(conn, stop_event)` prior to spawning worker threads,
   eliminating the race between accept, registration, and shutdown, and close
   active connections via `registry.close_all()` promptly on server exit to
   unblock worker threads and guarantee deterministic shutdown (< 1.5s).
   Restore previous signal handlers on exit.
   Sidecar test suite is wired across root `pyproject.toml`,
   `ai/pyproject.toml`, `noxfile.py`, and CI.

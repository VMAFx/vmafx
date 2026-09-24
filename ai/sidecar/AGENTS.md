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

6. **Unix socket permissions and pathname ownership (ADR-1309)** — The Unix
   domain socket is an unauthenticated endpoint and must be created with mode
   exactly `0o600` (`rw-------`) under the shipped same-UID contract. The current
   Helm chart does not wire a sidecar container or accept `sidecar.*` values, so
   do not reintroduce unconditional group access based on a hypothetical
   different-UID pod. A future group-shared mode requires an explicit setting,
   working chart wiring, threat-model documentation, and end-to-end coverage.
   Hold the adjacent owner-only `.lock` claim for the entire server lifetime;
   inspect paths with `lstat`/no-follow checks; remove a stale socket only after
   type plus device/inode identity remains unchanged; record the socket identity
   after bind; and remove it at shutdown only if that exact socket is still
   published. Symlinks, ordinary files, active listeners, and replacement
   sockets/files are never removed. Alert 946 is eliminated at source; hosted
   closure still depends on the post-merge Code Scanning run.

7. **Connection registry lifecycle** — Initialize `threads`,
   `registry = _ConnectionRegistry()`, `old_sigterm`, and `old_sigint` before
   `bind()` to avoid masking `OSError` with `UnboundLocalError`. Register accepted
   sockets through `register_if_active(conn, stop_event)` before spawning worker
   threads, and close active connections through `registry.close_all()` on exit
   to unblock workers and guarantee deterministic shutdown (< 1.5s). Restore
   previous signal handlers on exit. The sidecar suite remains wired across root
   `pyproject.toml`, `ai/pyproject.toml`, `noxfile.py`, and CI.

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
   and all downstream `onnxruntime-go` consumers. The PyTorch 2.14 exporter
   receives tuple arguments and `dynamic_shapes=({0: "batch"},)`; restoring
   legacy `dynamic_axes` emits a warning under the required dynamo exporter.
   Training flattens predictions and targets to equal-length vectors, rejects
   a count mismatch, and must keep batch-size-one steps warning-free.
   `OnlineTrainer.ingest()` derives the new-sample window as
   `batch_size - floor(batch_size * replay_mix_ratio)` and reserves exactly that
   many oldest pending samples; replay fills the rest without consuming later
   pending samples. Replay sampling uses replacement, so cold history or a
   replay capacity below the replay share still yields the configured batch
   size. One owner holds the complete reserve -> train ->
   restore/commit lifecycle, so a second complete window cannot train ahead of
   a failed first window. After either a `RuntimeError` or `ValueError`, the
   reserved window returns to the front before the owner is released. Concurrent
   arrivals remain queued behind it, bounded by `VMAFX_SIDECAR_PENDING_CAPACITY`
   (queued plus in-flight samples). A full queue rejects concurrent arrivals
   before replay admission; an idle full queue retries its oldest window and
   admits the triggering sample only after that step succeeds. A failed step
   whose triggering sample was already admitted returns `ok: true`,
   `trained: false`, and `retry_queued: true`; callers must not resubmit it. A
   capacity-deferred sample remains unaccepted when that retry fails, so the
   exception / `ok: false` ACK tells the caller to retry. Run the complete
   `ai/sidecar/tests` suite with warnings promoted to errors.

4. **Replay and pending capacity defaults** — both are 10 000 samples. The
   executable source (`_REPLAY_BUFFER_CAPACITY`, `_PENDING_CAPACITY`), focused
   tests, and the environment-variable table in
   `docs/ai/sidecar-online-training.md` are the current contract and move
   together. The pending capacity must fit one batch's new-sample window. The
   orphaned Helm helper mentions
   `sidecar.trainer.replayBufferSize`, but the chart neither schemas nor consumes
   that value; do not describe it as a supported Helm setting.

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
   inspect paths with `lstat`/no-follow checks; probe existing sockets in
   non-blocking mode; and treat only explicit `ECONNREFUSED` as stale.
   `EAGAIN`, `EINPROGRESS`, timeouts, and every other pending/unverified result
   mean `EADDRINUSE`, including a live listener with a full accept queue. Remove
   a stale socket only after type plus device/inode identity remains unchanged;
   record the socket identity after bind; and remove it at shutdown only if that
   exact socket is still published. Symlinks, ordinary files, active listeners,
   and replacement sockets/files are never removed. Alert 946 is eliminated at
   source; hosted closure still depends on the post-merge Code Scanning run.

7. **Connection registry lifecycle** — Initialize `threads`,
   `registry = _ConnectionRegistry()`, `old_sigterm`, and `old_sigint` before
   `bind()` to avoid masking `OSError` with `UnboundLocalError`. Register accepted
   sockets through `register_if_active(conn, stop_event)` before spawning worker
   threads, and close active connections through `registry.close_all()` on exit
   to unblock workers and guarantee deterministic shutdown (< 1.5s). Restore
   previous signal handlers on exit. The sidecar suite remains wired across root
   `pyproject.toml`, `ai/pyproject.toml`, `noxfile.py`, and CI.

8. **Standalone checkpoint directory requirement** — `OnlineTrainer` defaults
   `checkpoint_dir` to `/mnt/vmafx-models/online` (matching container/PVC mount
   topology). Because `/mnt` is root-owned and unwritable in normal standalone
   environments, any human-facing standalone sidecar example or integration
   test must explicitly allocate and configure `VMAFX_SIDECAR_CHECKPOINT_DIR`
   alongside `VMAFX_SIDECAR_SOCKET` with appropriate permissions and cleanup
   traps. Never rely on the production `/mnt` default for host-side quick-starts.

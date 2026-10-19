<!-- markdownlint-disable MD013 MD046 MD060 -->
# Sidecar Online Training

This page describes a Python server that trains an SGD + EMA model from
`(features, true_score)` messages, and the Go `FeedbackClient` that can send
them over a Unix socket. Both exist as components. They are not an end-to-end
product surface, so the shipped deployment does not continuously fine-tune its
scoring model.

!!! warning
    Nothing is wired together in this release:

    - No production scoring or executor path calls `FeedbackClient.Send`.
    - The Helm chart does not deploy the Python server: it has no trainer
      container, and `values.schema.json` rejects `sidecar.*` values. There is
      no supported Helm quick start.
    - `vmafx-node` does not consume the checkpoints the server exports.

    Treat the architecture below as an implemented component awaiting
    deployment wiring, not as a description of the current chart.

This surface is part of the VMAFX Phase 4b distributed platform (ADR-0781,
ADR-0709). It is separate from the on-host bias-correction sidecar used by
`vmaf-tune` (ADR-0394), a single-host, non-k8s, non-ONNX surface, see
[local-sidecar-training.md](local-sidecar-training.md).

## Run the server for development

The server needs a private runtime directory, a writable checkpoint directory
and a same-UID client.

1. Create a private runtime directory and a checkpoint directory:

    ```bash
    runtime_dir="$(mktemp -d)"
    trap 'rm -rf "$runtime_dir"' EXIT INT TERM
    chmod 700 "$runtime_dir"
    mkdir -p "$runtime_dir/checkpoints"
    ```

2. Export the socket and checkpoint paths:

    ```bash
    export VMAFX_SIDECAR_SOCKET="$runtime_dir/vmafx-sidecar.sock"
    export VMAFX_SIDECAR_CHECKPOINT_DIR="$runtime_dir/checkpoints"
    ```

3. Start the server:

    ```bash
    python -m ai.sidecar.online_trainer
    ```

The production default checkpoint directory (`/mnt/vmafx-models/online`) assumes
a container mount backed by a persistent volume. Standalone execution outside a
container fails with `PermissionError` on a root-owned `/mnt` unless
`VMAFX_SIDECAR_CHECKPOINT_DIR` points to a writable directory.

The server creates the unauthenticated socket and its adjacent lifetime-claim
file as `0o600`. It does not create or validate the parent directory, so the
operator owns the parent-directory security boundary. A future cross-UID or
shared-group deployment must add an explicit configuration contract, chart
wiring and end-to-end security tests. Changing the mode alone is not a supported
deployment mechanism.

## Architecture

```text
producer integration            Go transport              Python trainer
────────────────────            ────────────              ──────────────
not implemented today    X  FeedbackClient.Send()  ──→  ReplayBuffer (10 000)
                               non-blocking queue              │
                               (1 000 entries)                 │ batch (32,
                                                               │ 50% replay)
                                                               ↓
                                                          SGDEMATrainer.step()
                                                               │ EMA update
                                                               ↓
                                                          periodic ONNX export
                                                          + SHA-256 file
```

Design choices:

| Choice | Reason |
| --- | --- |
| Unix socket | The implemented Go and Python transports use a local Unix socket. A complete deployment needs both same-UID processes to share one private directory and identical `VMAFX_SIDECAR_SOCKET` values. No current chart render provides that shared mount or the Python process. |
| SGD + EMA | SGD has lower overhead per step than Adam for the tiny two-layer regression heads used here. The EMA shadow (Polyak averaging, `beta=0.999`) smooths noisy gradient steps from short-content bursts (ADR-0781 section Decision; Mean Teacher paper, 2017). |
| Replay buffer | Without replay, a wave of narrow-content jobs (for example one hour of HDR animation) overwrites the model's knowledge of other content. The 10 000-sample ring buffer is about 3.2 MB of raw 80-float payload before Python object and container overhead, and retains about 200 hours of a hypothetical 50-sample/hour stream. |

### Batching and replay

For a configured batch size `B` and replay fraction `R`:

- Each gradient step reserves exactly the oldest $B - \lfloor B R \rfloor$ pending
  samples.
- Replay fills the remaining slots without replacement when history is
  sufficient, and with replacement only when the available history is smaller
  than that share.
- Only the reserved new samples are consumed. With a 50% replay mix and batch
  size 4, pending samples 1 and 2 train before samples 3 and 4.
- The reserve, train, and restore-or-commit lifecycle has one owner. Concurrent
  ingests queue behind it and cannot start a second step.

Only the reserved new-sample count advances the checkpoint sample gate. Replay
rows never make a checkpoint eligible.

### Backpressure

`VMAFX_SIDECAR_PENDING_CAPACITY` caps the admitted pending backlog (queued plus
in-flight new samples):

| Situation | Server response | Client behaviour |
| --- | --- | --- |
| The backlog is full while a step is running | Fails with `pending training queue is full ...; retry later` before the sample enters either queue | Retry later |
| The queue is full and idle after a failed step | Retries the oldest window first; admits the new sample only if that retry succeeds | See the ACK shapes below |
| A step raises `RuntimeError` or `ValueError` | Restores the reserved samples to the front of the queue, keeping FIFO retry order with no silent loss | None |

`OnlineTrainer.status()` exposes `pending_size`, `pending_capacity` and
`training_active` to embedding callers. The standalone process provides no HTTP
status endpoint.

### ACK shapes

An ACK distinguishes admission from training success.

1. Admitted and trained. `checkpoint` is the exported path when this step
   triggered a checkpoint, otherwise `null`:

    ```json
    {"ok": true, "step": 1234, "trained": true, "loss": 0.42, "checkpoint": null}
    ```

2. Admitted, with no step run yet (the batch is not full):

    ```json
    {"ok": true, "step": 1234, "trained": false}
    ```

3. Admitted, but the training step failed. The sample is already in the restored
   FIFO window, so the caller must not submit it again. The sidecar logs the
   failure. The Go `FeedbackClient` counts this ACK as delivered and logs the
   deferred error:

    ```json
    {"ok": true, "trained": false, "retry_queued": true, "training_error": "..."}
    ```

4. Not admitted because capacity was full and the oldest-window retry failed.
   The
   Go client keeps the unaccepted in-flight sample locally, reconnects, and
   retries it before reading the bounded queue. The sample cannot be lost when a
   concurrent sender refills the queue, and `delivered` does not increment until
   the retry succeeds:

    ```json
    {"ok": false, "retryable": true, "error": "..."}
    ```

Malformed or otherwise non-retryable input does not carry `retryable: true`.
This split prevents both silent loss and duplicate feedback. A local JSON
encoding failure (for example a non-finite feature or score) is permanent
instead: the client increments `Dropped()`, keeps the connection open and keeps
draining, so invalid feedback cannot starve later valid samples.

## Configuration reference

The executable reads environment variables directly. They are not wired to
supported Helm values in the current chart.

| Env var | Default | Description |
|---|---|---|
| `VMAFX_BASE_MODEL_PATH` | empty | Python trainer seed model. A PyTorch state dict matching the fallback two-layer MLP loads directly; ONNX loading needs the optional `onnx2torch` module. A missing, inaccessible or unsupported model falls back to a new two-layer MLP. |
| `VMAFX_SIDECAR_SOCKET` | `/tmp/vmafx-sidecar.sock` | Unix socket path. The default matches the Go client. |
| `VMAFX_SIDECAR_CHECKPOINT_DIR` | `/mnt/vmafx-models/online` | Directory for versioned ONNX checkpoint output. |
| `VMAFX_SIDECAR_REPLAY_CAPACITY` | `10000` | Replay buffer capacity (samples). |
| `VMAFX_SIDECAR_PENDING_CAPACITY` | `10000` | Maximum admitted new samples not yet committed by a successful step, counting queued and in-flight samples. Must fit one batch's new-sample portion. |
| `VMAFX_SIDECAR_BATCH_SIZE` | `32` | Mini-batch size per gradient step. |
| `VMAFX_SIDECAR_REPLAY_MIX` | `0.5` | Fraction of each batch drawn from the replay buffer, in `[0.0, 1.0)`. |
| `VMAFX_SIDECAR_LR` | `0.0001` | SGD learning rate used by the executable. |
| `VMAFX_SIDECAR_EMA_DECAY` | `0.999` | EMA decay beta. |
| `VMAFX_SIDECAR_CKPT_INTERVAL_S` | `600` | Minimum seconds between checkpoints. |
| `VMAFX_SIDECAR_MIN_SAMPLES_CKPT` | `1000` | Minimum successfully trained new samples before a checkpoint; replay rows are excluded. |
| `VMAFX_SIDECAR_N_FEATURES` | `80` | Feature vector dimension from vmafx-node. |

### Socket and checkpoint paths

For standalone or future production use, set the same `VMAFX_SIDECAR_SOCKET` in
both processes and place it below an owner-only runtime directory. Socket mode
alone does not secure a writable parent directory.

When an endpoint already exists, startup probes it in non-blocking mode:

- Only an explicit `ECONNREFUSED` counts as stale.
- Queue pressure (`EAGAIN`), a pending connection, a timeout or any other
  unverified result fails closed as `EADDRINUSE`.
- A live listener with a full accept queue is never unlinked.

`VMAFX_SIDECAR_CHECKPOINT_DIR` defaults to `/mnt/vmafx-models/online` for
container deployments with persistent volumes. Standalone invocations must set
it to a writable path to avoid a startup `PermissionError`.

## Checkpoint format

Each checkpoint export writes two files atomically:

```text
/mnt/vmafx-models/online/model_v000042.onnx         # EMA model
/mnt/vmafx-models/online/model_v000042.onnx.sha256  # SHA-256 digest
```

The ONNX file uses opset 17 (matching ADR-0249 and the rest of the tiny-AI
export stack). Input shape is `(batch, n_features)` and output shape is
`(batch, 1)`. Both axes named `batch` are dynamic in the exported graph,
including when a checkpoint follows a one-sample training step. Training treats
predictions and targets as equal-length vectors and rejects mismatched sample
counts instead of letting PyTorch broadcast them.

No current `vmafx-node` path discovers or loads these files. Setting
`VMAFX_BASE_MODEL_PATH` for a later trainer process seeds that trainer from the
checkpoint when the required loader is available. Promoting an exported
checkpoint into a scoring model is an external, manual integration step today.

## Kubernetes status

The chart has no trainer container. An unused `sidecar-trainer.yaml` helper,
which no workload included, was removed; the values schema exposes no
`sidecar.*` configuration.

The `VmafxModelTraining` CRD and an operator-side status mapper also exist. The
mapper polls an HTTP `/status` service that this Python server does not provide,
and the chart creates no trainer Service. Creating the CR therefore does not
deploy, drive or observe this trainer.

## Go-side metrics

`FeedbackClient` exposes two in-memory counters through Go methods:

| Counter | Description |
|---|---|
| `Dropped()` | Messages rejected because the in-memory queue was full, plus permanent local JSON encoding failures. A non-retryable sidecar rejection is terminal but is not a local drop. |
| `Delivered()` | Messages acknowledged by the Python server. |

The node logs both values when it stops the drainer. They are not registered as
Prometheus metrics, and with no production caller of `Send` they stay zero in
the
shipped scoring flow.

## Limitations (v1)

- **No automatic feedback producer.** The client and server transports exist,
  but scoring and executor code do not call `FeedbackClient.Send`.
- **No checkpoint consumption.** The node neither verifies nor loads trainer
  output, on restart or at runtime.
- **CPU-only trainer.** The implementation creates CPU tensors and has no
  `VMAFX_SIDECAR_CUDA` setting or other device-selection surface.
- **Single base model per sidecar.** Per-tenant adapter heads (LoRA) are
  deferred
  to v2.
- **No stability gate and no quarantine.** Nothing scores an exported checkpoint
  against a fixture set or tags it as stable. Any external consumer must
  validate
  and promote it explicitly, see the next section.

## Checkpoint quarantine (not implemented)

[Research-0733 section 3.4](../research/0733-vmafx-sidecar-training-architecture.md)
specifies a stability gate and a three-way version-selection policy. None of it
exists in the tree. This section records the split so nobody plans against a
surface that is not there.

### What section 3.4 specifies

After each checkpoint is committed, the controller scores an internal fixture
set (10 to 20 reference/distorted pairs with known VMAF scores) and compares
PLCC
against the previous checkpoint. A regression larger than `stability_plcc_delta`
(default 0.005) tags the checkpoint `unstable` and excludes it from the
`latest-stable` selection policy. `VmafxModelTraining` carries a
`spec.versionPolicy` field with three values, `latest`, `latest-stable` and
`pinned:<version>`, defaulting to `latest-stable`.

### What is in the tree

| Section 3.4 element | State |
| --- | --- |
| Atomic checkpoint write (temp file + `os.replace`) | Implemented. `SGDEMATrainer.export_onnx` exports to a `mkstemp` `.tmp.onnx` and renames; the `.sha256` file is written the same way. |
| `model.onnx.sha256` digest sidecar written | Implemented. `_write_sha256_sidecar` in [`ai/sidecar/online_trainer.py`](../../ai/sidecar/online_trainer.py). |
| Digest verified by the node before load | Not written. No SHA-256 check exists in `cmd/vmafx-node/`; the file is produced and nothing consumes it. |
| `status.modelVersion` propagated to the CR | Type and mapping code exist but are disconnected: the controller expects an HTTP trainer service that the Python server and chart do not provide. |
| `spec.versionPolicy` field | Absent from the CRD. No `latest`, `latest-stable` or `pinned:` selection exists. |
| Fixture set for the stability gate | Does not exist. |
| PLCC comparison job in the controller | Not written. |
| `unstable` tag or quarantine on a regressing checkpoint | Not written. No checkpoint is ever withheld. |
| `stability_plcc_delta` knob | Not a field anywhere. |
| Automatic rollback on `vmaf_score_pooled` failure rate | Not written. The `rollback_threshold` in section 3.4 is a proposal. |

!!! warning
    Treat every exported checkpoint as an unvetted standalone artefact. Gate it
    outside the sidecar before adapting it for any scoring consumer, and keep
    the
    previous scoring model reachable for rollback.
    [ADR-0781](../adr/0781-sidecar-sgd-ema-online-trainer.md) section
    Limitations records the same deferral from the design side.

## See also

- [ADR-0781](../adr/0781-sidecar-sgd-ema-online-trainer.md): design rationale
- [Research-0733](../research/0733-vmafx-sidecar-training-architecture.md):
  architecture evaluation
- [ADR-0394](../adr/0394-local-sidecar-training.md): on-host vmaf-tune predictor
  sidecar (different surface)
- [ADR-0249](../adr/0249-fr-regressor-v1.md): ONNX export opset constraints
- [local-sidecar-training.md](local-sidecar-training.md): vmaf-tune on-host
  ridge sidecar

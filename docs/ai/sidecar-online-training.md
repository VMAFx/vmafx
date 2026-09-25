<!-- markdownlint-disable MD013 MD060 -->
# Sidecar Online Training

The repository contains two building blocks for online training: a Go
`FeedbackClient` that can send `(features, true_score)` messages over a Unix
socket, and a Python server that can train an SGD + EMA model from those
messages. They are not an end-to-end product surface today. No production
scoring or executor path calls `FeedbackClient.Send`, the Helm chart does not
deploy the Python server, and `vmafx-node` does not consume the checkpoints it
exports. Consequently, the shipped deployment does not continuously fine-tune
its scoring model.

This surface is part of the VMAFX Phase 4b distributed platform
(ADR-0781, ADR-0709).  It is separate from the on-host bias-correction
sidecar used by `vmaf-tune` (ADR-0394), which is a single-host, non-k8s,
non-ONNX surface.

---

## Deployment status

The Python server and Go feedback client exist, but the current Helm chart does
**not** deploy them together. `templates/sidecar-trainer.yaml` defines a named
helper whose recorded consumer (`node-deployment.yaml`) no longer exists;
`templates/node.yaml` does not include the helper; and `values.schema.json`
rejects `sidecar.*` values. There is therefore no supported Helm quick start in
this release. Treat the architecture below as an implemented component awaiting
deployment wiring, not as a description of the current chart.

For isolated development, start the server directly after arranging a private
runtime directory, a writable checkpoint directory, and a same-UID client:

```bash
runtime_dir="$(mktemp -d)"
trap 'rm -rf "$runtime_dir"' EXIT INT TERM
chmod 700 "$runtime_dir"
mkdir -p "$runtime_dir/checkpoints"

VMAFX_SIDECAR_SOCKET="$runtime_dir/vmafx-sidecar.sock" \
VMAFX_SIDECAR_CHECKPOINT_DIR="$runtime_dir/checkpoints" \
  python -m ai.sidecar.online_trainer
```

The production default checkpoint directory (`/mnt/vmafx-models/online`) assumes
a container mount backed by a persistent volume; standalone execution outside a
container will fail with `PermissionError` on a root-owned `/mnt` unless
`VMAFX_SIDECAR_CHECKPOINT_DIR` points to a writable directory.

The server creates the unauthenticated socket and its adjacent lifetime-claim
file as `0o600`; it does not create or validate the parent directory. The
operator therefore owns the parent-directory security boundary. A future
cross-UID/group-shared deployment must add an explicit configuration contract,
chart wiring, and end-to-end security tests; changing the mode alone is not a
supported deployment mechanism.

---

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

**Why Unix socket?** The implemented Go and Python transports use a local Unix
socket. A complete deployment would need to give both same-UID processes the
same private directory and identical `VMAFX_SIDECAR_SOCKET` values. No current
chart render provides that shared mount or the Python process.

**Why SGD + EMA?**  Standard SGD is lower overhead per step than Adam for
the tiny two-layer regression heads used here.  The EMA shadow
(Polyak averaging, `beta=0.999`) smooths out noisy gradient steps from
short-content bursts (ADR-0781 §Decision; Mean Teacher paper, 2017).

**Why a replay buffer?**  Without replay, a sudden wave of narrow-content
jobs (e.g., one hour of HDR animation) overwrites the model's knowledge of
other content types. The 10 000-sample ring buffer represents about 3.2 MB of
raw 80-float payload before Python object and container overhead, and retains
about 200 hours of a hypothetical 50-sample/hour input stream.

For a configured batch size `B` and replay fraction `R`, each gradient step
reserves exactly the oldest `B - floor(B * R)` pending samples; replay fills the
remaining slots without replacement when history is sufficient and with
replacement only when the available history is smaller than that share. Only
those reserved new samples are consumed, so a 50% replay mix with batch size 4
trains pending samples 1 and 2 before samples 3 and 4.
The reserve, train, and restore-or-commit lifecycle has one owner. Concurrent
ingests can enqueue behind that owner, but cannot start a second step.

The admitted pending backlog (queued plus in-flight new samples) is capped by
`VMAFX_SIDECAR_PENDING_CAPACITY`. When it is full during a step, another ingest
fails explicitly with `pending training queue is full ...; retry later` before
its sample enters either queue. When the queue is full and idle after a failed
step, the next ingest retries the oldest window first and admits its new sample
only if that retry succeeds. A `RuntimeError` or `ValueError` restores the
reserved samples to the front, preserving FIFO retry order without silent loss.
Only the reserved new-sample count advances the checkpoint sample gate; replay
rows never make a checkpoint eligible.
`OnlineTrainer.status()` exposes `pending_size`, `pending_capacity`, and
`training_active` to embedding callers; the standalone process still does not
provide an HTTP status endpoint.

ACKs distinguish admission from training success. If the call's sample was
already admitted before a step failed, the sidecar logs the failure and returns
`{"ok": true, "trained": false, "retry_queued": true,
"training_error": "..."}`. The sample is already in the restored FIFO window;
the caller must not submit it again. The Go `FeedbackClient` counts that ACK as
delivered and logs the deferred training error. If capacity prevented admission,
a failed oldest-window retry returns
`{"ok": false, "retryable": true, "error": "..."}`. The Go client retains
that unaccepted in-flight sample locally, reconnects, and retries it before
reading the bounded queue. It therefore cannot be lost when a concurrent sender
refills the queue, and `delivered` does not increment until the retry succeeds.
Malformed or otherwise non-retryable input does not carry `retryable: true`.
This distinction prevents both silent loss and duplicate feedback. A local JSON
encoding failure (for example a non-finite feature or score) is instead
permanent: the client increments `Dropped()`, keeps the connection open, and
continues draining so that invalid feedback cannot starve later valid samples.

---

## Configuration reference

The executable currently accepts environment variables directly. These are not
wired to supported Helm values in the current chart.

| Env var | Default | Description |
|---|---|---|
| `VMAFX_BASE_MODEL_PATH` | empty | Python trainer seed model. A PyTorch state dict matching the fallback two-layer MLP is loaded directly; ONNX loading needs the optional `onnx2torch` module. A missing, inaccessible, or unsupported model falls back to a new two-layer MLP. |
| `VMAFX_SIDECAR_CHECKPOINT_DIR` | `/mnt/vmafx-models/online` | Directory for versioned ONNX checkpoint output. |
| `VMAFX_SIDECAR_REPLAY_CAPACITY` | `10000` | Replay buffer capacity (samples). |
| `VMAFX_SIDECAR_PENDING_CAPACITY` | `10000` | Maximum admitted new samples not yet committed by a successful step, counting queued and in-flight samples. Must fit one batch's new-sample portion. |
| `VMAFX_SIDECAR_BATCH_SIZE` | `32` | Mini-batch size per gradient step. |
| `VMAFX_SIDECAR_REPLAY_MIX` | `0.5` | Fraction of each batch drawn from replay buffer, in `[0.0, 1.0)`. |
| `VMAFX_SIDECAR_LR` | `0.0001` | SGD learning rate used by the executable. |
| `VMAFX_SIDECAR_EMA_DECAY` | `0.999` | EMA decay beta. |
| `VMAFX_SIDECAR_CKPT_INTERVAL_S` | `600` | Minimum seconds between checkpoints. |
| `VMAFX_SIDECAR_MIN_SAMPLES_CKPT` | `1000` | Minimum successfully trained new samples before checkpoint; replay rows are excluded. |
| `VMAFX_SIDECAR_N_FEATURES` | `80` | Feature vector dimension from vmafx-node. |

The socket path (`VMAFX_SIDECAR_SOCKET`) defaults to `/tmp/vmafx-sidecar.sock`
for compatibility with the Go client. For standalone or future production use,
set the same override in both processes and place it below an owner-only runtime
directory; socket mode alone does not secure a writable parent directory.
When an endpoint already exists, startup probes it in non-blocking mode and
treats only explicit `ECONNREFUSED` as stale. Queue pressure (`EAGAIN`), a
pending connection, a timeout, or any other unverified result fails closed as
`EADDRINUSE`; a live listener with a full accept queue is never unlinked.
Similarly, `VMAFX_SIDECAR_CHECKPOINT_DIR` defaults to `/mnt/vmafx-models/online`
for container deployments with persistent volume mounts; standalone invocations
must set `VMAFX_SIDECAR_CHECKPOINT_DIR` to a writable path to avoid startup
`PermissionError` failures.

---

## Checkpoint format

Each checkpoint export writes two files atomically:

```text
/mnt/vmafx-models/online/model_v000042.onnx         # EMA model
/mnt/vmafx-models/online/model_v000042.onnx.sha256  # SHA-256 digest
```

The ONNX file uses opset 17 (matching ADR-0249 and the rest of the tiny-AI
export stack).  Input shape: `(batch, n_features)`.  Output shape: `(batch, 1)`.
Both axes named `batch` are dynamic in the exported graph, including when a
checkpoint follows a one-sample training step. Training treats predictions and
targets as equal-length vectors and rejects mismatched sample counts instead of
letting PyTorch broadcast them.

No current `vmafx-node` path discovers or loads these files. The
`VMAFX_BASE_MODEL_PATH` variable belongs to the Python trainer: setting it for a
later trainer process seeds that trainer from the checkpoint when the required
loader is available. Promoting an exported checkpoint into a scoring model is
an external, manual integration step today.

---

## Kubernetes status (not currently wired)

The orphaned `sidecar-trainer.yaml` helper contains proposed container,
environment, volume, probe, and restart fields, but no workload includes it and
the values schema exposes no corresponding configuration. It is not a rendered
or validated deployment contract.

The `VmafxModelTraining` CRD and an operator-side status mapper also exist, but
the mapper polls an HTTP `/status` service that this Python server does not
provide, and the chart creates no trainer Service. Creating the CR therefore
does not deploy, drive, or observe this trainer.

---

## Go-side metrics

`FeedbackClient` exposes two in-memory counters through Go methods:

| Counter | Description |
|---|---|
| `Dropped()` | Messages rejected because the in-memory queue was full, plus permanent local JSON encoding failures. A non-retryable sidecar rejection is terminal but is not a local drop. |
| `Delivered()` | Messages acknowledged by the Python server. |

The node logs both values when it stops the drainer. They are not registered as
Prometheus metrics, and with no production caller of `Send` they remain zero in
the shipped scoring flow.

---

## Limitations (v1)

- **No automatic feedback producer**: the client and server transports exist,
  but scoring and executor code do not call `FeedbackClient.Send`.
- **No checkpoint consumption**: the node neither verifies nor loads trainer
  output, on restart or at runtime.
- **CPU-only trainer**: the implementation creates CPU tensors and has no
  `VMAFX_SIDECAR_CUDA` setting or other device-selection surface.
- **Single base model per sidecar**: per-tenant adapter heads (LoRA) are
  deferred to v2.
- **No stability gate, and no quarantine**: nothing scores an exported
  checkpoint against a fixture set or tags it as stable. Any external consumer
  must validate and promote it explicitly. See
  [Checkpoint quarantine](#checkpoint-quarantine-not-implemented) below for
  what is and is not in the tree.

---

## Checkpoint quarantine (not implemented)

[Research-0733 §3.4](../research/0733-vmafx-sidecar-training-architecture.md)
specifies a stability gate and a three-way version-selection policy. **None of
it exists in the tree.** This section records the split so nobody plans against
a surface that is not there.

**What §3.4 specifies.** After each checkpoint is committed, the controller
scores an internal fixture set (10–20 reference/distorted pairs with known VMAF
scores) and compares PLCC against the previous checkpoint. A regression larger
than `stability_plcc_delta` (default 0.005) tags the checkpoint `unstable` and
excludes it from the `latest-stable` selection policy. `VmafxModelTraining`
carries a `spec.versionPolicy` field with three values — `latest`,
`latest-stable`, `pinned:<version>` — defaulting to `latest-stable`.

**What is actually in the tree.**

| §3.4 element | State |
| --- | --- |
| Atomic checkpoint write (temp file + `os.replace`) | Implemented — `SGDEMATrainer.export_onnx` exports to a `mkstemp` `.tmp.onnx` and renames; the `.sha256` file is written the same way. |
| `model.onnx.sha256` digest sidecar written | Implemented — `_write_sha256_sidecar` in [`ai/sidecar/online_trainer.py`](../../ai/sidecar/online_trainer.py). |
| Digest **verified by the node before load** | **Not written** — no SHA-256 check exists anywhere in `cmd/vmafx-node/`. The file is produced but nothing consumes it. |
| `status.modelVersion` propagated to the CR | Type and mapping code exist, but are disconnected: the controller expects an HTTP trainer service that the Python server and chart do not provide. |
| `spec.versionPolicy` field | **Absent** from the CRD — no `latest` / `latest-stable` / `pinned:` selection exists. |
| Fixture set for the stability gate | **Does not exist.** |
| PLCC comparison job in the controller | **Not written.** |
| `unstable` tag / quarantine on a regressing checkpoint | **Not written.** No checkpoint is ever withheld. |
| `stability_plcc_delta` knob | **Not a field anywhere.** |
| Automatic rollback on `vmaf_score_pooled` failure rate | **Not written**; the `rollback_threshold` in §3.4 is a proposal. |

**Consequence for operators.** Treat every exported checkpoint as an unvetted
standalone artefact. Gate it outside the sidecar before adapting it for any
scoring consumer, and keep the previous scoring model reachable for rollback.
[ADR-0781](../adr/0781-sidecar-sgd-ema-online-trainer.md) §Limitations records
the same deferral from the design side.

## See also

- [ADR-0781](../adr/0781-sidecar-sgd-ema-online-trainer.md) — design rationale
- [Research-0733](../research/0733-vmafx-sidecar-training-architecture.md) — architecture evaluation
- [ADR-0394](../adr/0394-local-sidecar-training.md) — on-host vmaf-tune predictor sidecar (different surface)
- [ADR-0249](../adr/0249-fr-regressor-v1.md) — ONNX export opset constraints
- [docs/ai/local-sidecar-training.md](local-sidecar-training.md) — vmaf-tune on-host ridge sidecar

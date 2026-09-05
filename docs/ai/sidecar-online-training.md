<!-- markdownlint-disable MD013 MD060 -->
# Sidecar Online Training

The VMAFX sidecar trainer continuously fine-tunes a tiny-AI ONNX model
while `vmafx-node` processes encoding jobs.  Every scored job contributes
a `(features, true_score)` training pair that is shipped to a co-located
Python sidecar container over a Unix socket.  No round-trip to a training
cluster is required; checkpoints are available within minutes.

This surface is part of the VMAFX Phase 4b distributed platform
(ADR-0781, ADR-0709).  It is separate from the on-host bias-correction
sidecar used by `vmaf-tune` (ADR-0394), which is a single-host, non-k8s,
non-ONNX surface.

---

## Quick start

Enable the sidecar in your Helm values:

```yaml
sidecar:
  trainer:
    enabled: true
    image:
      repository: ghcr.io/vmafx/vmafx-sidecar
      tag: latest
    baseModelPath: /mnt/vmafx-models/base/fr_regressor_v3.onnx
    checkpointDir: /mnt/vmafx-models/online
    nFeatures: 80
    resources:
      requests:
        cpu: "0.5"
        memory: "512Mi"
      limits:
        cpu: "2"
        memory: "1Gi"

persistence:
  models:
    enabled: true
    mountPath: /mnt/vmafx-models
```

The sidecar container starts alongside `vmafx-node` in the same pod, binds
`/tmp/vmafx-sidecar.sock`, and begins accepting training pairs immediately.

---

## Architecture

```text
vmafx-node (Go)               vmafx-sidecar (Python)
─────────────────             ──────────────────────────
scorer.Score(ref, dis)   ──→  ReplayBuffer (10 000 cap)
  │ FeedbackClient.Send()         │
  │ (non-blocking, 1000-entry     │ batch (32 samples, 50% replay)
  │  ring buffer)                 ↓
  │                          SGDEMATrainer.step()
  │                              │ EMA update (β=0.999)
  │                              │
  │                        every 10 min + 1000 new samples:
  │                          export_onnx() → /mnt/vmafx-models/online/
  │                          write SHA-256 sidecar
  │
  ↓ next restart: node loads new ONNX checkpoint
```

**Why Unix socket?**  The `vmafx-node` and `vmafx-sidecar` containers share
an `emptyDir` volume mounted at `/tmp`.  Unix domain sockets over a shared
volume have lower per-message overhead than TCP loopback for the expected
message rate (< 100 pairs/s) and avoid an extra container port declaration.

**Why SGD + EMA?**  Standard SGD is lower overhead per step than Adam for
the tiny two-layer regression heads used here.  The EMA shadow
(Polyak averaging, `beta=0.999`) smooths out noisy gradient steps from
short-content bursts (ADR-0781 §Decision; Mean Teacher paper, 2017).

**Why a replay buffer?**  Without replay, a sudden wave of narrow-content
jobs (e.g., one hour of HDR animation) overwrites the model's knowledge of
other content types.  The 10 000-sample ring buffer (≈ 3.2 MB at 80 float32
features/sample) retains ~200 hours of a 50-encode/hour workload.

---

## Configuration reference

All settings are Helm values under `sidecar.trainer.*` and are passed to
the sidecar container as environment variables.

| Helm key | Env var | Default | Description |
|---|---|---|---|
| `enabled` | — | `false` | Enable the sidecar container. |
| `image.repository` | — | — | Sidecar container image. |
| `image.tag` | — | chart AppVersion | Image tag. |
| `baseModelPath` | `VMAFX_BASE_MODEL_PATH` | `/mnt/vmafx-models/base/model.onnx` | ONNX or PyTorch state-dict to fine-tune. |
| `checkpointDir` | `VMAFX_SIDECAR_CHECKPOINT_DIR` | `/mnt/vmafx-models/online` | Directory for versioned ONNX checkpoint output. |
| `replayBufferSize` | `VMAFX_SIDECAR_REPLAY_CAPACITY` | `10000` | Replay buffer capacity (samples). |
| `batchSize` | `VMAFX_SIDECAR_BATCH_SIZE` | `32` | Mini-batch size per gradient step. |
| `replayMixRatio` | `VMAFX_SIDECAR_REPLAY_MIX` | `0.5` | Fraction of each batch drawn from replay buffer. |
| `learningRate` | `VMAFX_SIDECAR_LR` | `0.0001` | SGD/Adam learning rate. |
| `emaDecay` | `VMAFX_SIDECAR_EMA_DECAY` | `0.999` | EMA decay beta. |
| `checkpointIntervalSeconds` | `VMAFX_SIDECAR_CKPT_INTERVAL_S` | `600` | Minimum seconds between checkpoints. |
| `minSamplesPerCheckpoint` | `VMAFX_SIDECAR_MIN_SAMPLES_CKPT` | `1000` | Minimum new samples before checkpoint. |
| `nFeatures` | `VMAFX_SIDECAR_N_FEATURES` | `80` | Feature vector dimension from vmafx-node. |

The socket path (`VMAFX_SIDECAR_SOCKET`) defaults to `/tmp/vmafx-sidecar.sock`
and should not be changed unless the volume mount path also changes.

---

## Checkpoint format

Each checkpoint export writes two files atomically:

```text
/mnt/vmafx-models/online/model_v000042.onnx         # EMA model
/mnt/vmafx-models/online/model_v000042.onnx.sha256  # SHA-256 digest
```

The ONNX file uses opset 17 (matching ADR-0249 and the rest of the tiny-AI
export stack).  Input shape: `(batch, n_features)`.  Output shape: `(batch, 1)`.

On next `vmafx-node` restart, set `VMAFX_BASE_MODEL_PATH` (or the Helm
`baseModelPath` value) to the new checkpoint path to pick it up.  Automated
live hot-reload is a follow-up (requires the `VmafxModelTraining` CRD,
Research-0733 §3.4).

---

## Kubernetes sidecar lifecycle (k8s 1.29+)

On clusters running Kubernetes 1.29 or later the sidecar container is
declared with `restartPolicy: Always` (native sidecar KEP-753 semantics).
This means:

- The sidecar starts before the main `vmafx-node` container.
- The pod's `Ready` condition waits for the sidecar's readiness probe to
  pass (Unix socket bound).
- If the sidecar crashes it is restarted without restarting `vmafx-node`.

On older clusters (`restartPolicy: Always` is ignored) the sidecar behaves
as a regular container; a crash restarts the whole pod.

---

## Go-side metrics

`FeedbackClient` exposes two counters available via the node's Prometheus
metrics endpoint (when instrumented):

| Counter | Description |
|---|---|
| `vmafx_training_feedback_dropped_total` | Messages dropped due to queue overflow or sidecar unavailability. |
| `vmafx_training_feedback_delivered_total` | Messages successfully delivered to the sidecar. |

A non-zero `dropped` counter means the sidecar is behind the scoring rate.
Increase `resources.limits.cpu` for the sidecar or reduce `batchSize`.

---

## Limitations (v1)

- **No live hot-reload**: the node loads the new checkpoint only on restart.
  The `VmafxModelTraining` CRD + controller (atomic session swap via
  `atomic.Pointer`) is the follow-up.
- **CPU training only by default**: CUDA training available via
  `VMAFX_SIDECAR_CUDA=1` but requires the node pod to have excess GPU budget.
- **Single base model per sidecar**: per-tenant adapter heads (LoRA) are
  deferred to v2.
- **No stability gate, and no quarantine**: every checkpoint the sidecar
  commits is immediately eligible for pickup. Nothing scores it against a
  fixture set, nothing tags it, and nothing withholds it — a checkpoint that
  regresses quality is picked up exactly like one that improves it. Recovery is
  manual: pin the node to a known-good version or stop the training session.
  See [Checkpoint quarantine](#checkpoint-quarantine-not-implemented) below for
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
| Atomic checkpoint write (temp file + `os.replace`) | Implemented — `SgdEmaTrainer.export_onnx` exports to a `mkstemp` `.tmp.onnx` and renames; the `.sha256` file is written the same way. |
| `model.onnx.sha256` digest sidecar written | Implemented — `_write_sha256_sidecar` in [`ai/sidecar/online_trainer.py`](../../ai/sidecar/online_trainer.py). |
| Digest **verified by the node before load** | **Not written** — no SHA-256 check exists anywhere in `cmd/vmafx-node/`. The file is produced but nothing consumes it. |
| `status.modelVersion` propagated to the CR | Implemented — [`api/vmafx/v1/vmafxmodeltraining_types.go`](../../api/vmafx/v1/vmafxmodeltraining_types.go), applied in `vmafxmodeltraining_controller.go`. |
| `spec.versionPolicy` field | **Absent** from the CRD — no `latest` / `latest-stable` / `pinned:` selection exists. |
| Fixture set for the stability gate | **Does not exist.** |
| PLCC comparison job in the controller | **Not written.** |
| `unstable` tag / quarantine on a regressing checkpoint | **Not written.** No checkpoint is ever withheld. |
| `stability_plcc_delta` knob | **Not a field anywhere.** |
| Automatic rollback on `vmaf_score_pooled` failure rate | **Not written**; the `rollback_threshold` in §3.4 is a proposal. |

**Consequence for operators.** Treat every committed checkpoint as unvetted.
If quality regression matters for a deployment, gate it outside the sidecar —
score the checkpoint yourself before pointing nodes at it, and keep the
previous version reachable so you can pin back to it.
[ADR-0781](../adr/0781-sidecar-sgd-ema-online-trainer.md) §Limitations records
the same deferral from the design side.

## See also

- [ADR-0781](../adr/0781-sidecar-sgd-ema-online-trainer.md) — design rationale
- [Research-0733](../research/0733-vmafx-sidecar-training-architecture.md) — architecture evaluation
- [ADR-0394](../adr/0394-local-sidecar-training.md) — on-host vmaf-tune predictor sidecar (different surface)
- [ADR-0249](../adr/0249-fr-regressor-v1.md) — ONNX export opset constraints
- [docs/ai/local-sidecar-training.md](local-sidecar-training.md) — vmaf-tune on-host ridge sidecar

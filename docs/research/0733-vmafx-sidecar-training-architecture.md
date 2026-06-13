<!-- markdownlint-disable MD013 MD049 MD060 -->
# Research-0733 — VMAFX Sidecar Online Training Architecture

_Date: 2026-05-28_
_Author: lusoris (AI-assisted, Claude Code)_
_Status: Accepted — accompanies ADR-0709 Phase 4b.7 implementation planning_
_Parent ADR: [ADR-0709](../adr/0709-vmafx-phase4b-distributed-platform.md) (Phase 4b umbrella, item 4b.7)_
_Tags: online-learning, sidecar, pytorch, lightning, grpc, onnx, k8s, training, operator, crd_

---

## 1. Background and Motivation

VMAFX Phase 4b (ADR-0709) transforms the platform from a single-binary scoring tool into a
distributed cloud-native platform. Every scoring job a `vmafx-node` executes is also an
implicit training event: the node encodes a distorted stream, scores it with libvmaf to obtain
a `predicted_score`, and already has the reference and distorted pixels in memory. The delta
between `predicted_score` and any available subjective ground truth (or a higher-fidelity
reference model score) constitutes a training signal. Discarding that signal after each job is
a missed opportunity.

The existing training path (`ai/scripts/` + PyTorch + Lightning) is batch-oriented: a human
assembles a corpus, runs a training script, exports an ONNX checkpoint, and deploys it. This
round-trip takes hours to days. The sidecar architecture closes the encode-score-train loop to
minutes, enabling the platform's models to continuously refine as they process real workloads.

The user direction that motivates this work:
> "ml training in python only is wrong as well — we want sidecar training while encoding etc."
> (ADR-0709 references, 2026-05-28)

---

## 2. Architecture Option Evaluation

### Option A — Python Sidecar Container Per Node (Recommended)

Each `vmafx-node` pod runs a co-located Python sidecar container within the same Kubernetes
pod. The Go node captures `(reference_features, distorted_features, predicted_score, metadata)`
triples and ships them to the sidecar via gRPC over loopback. The sidecar runs a PyTorch +
Lightning online learning loop. When a checkpoint threshold is met, the sidecar exports an
ONNX model and pushes it to the model registry (PVC mount or S3 via rclone).

**Advantages:**

- Reuses the existing PyTorch + Lightning stack from `ai/`. No new ML framework.
- In-pod latency: data-to-training is sub-100 ms.
- No network round-trip for training data.
- Pod-level isolation: a crashing sidecar does not take down the scoring node.
- Resource accounting is straightforward: sidecar resource limits are declared separately in
  the pod spec (`resources.limits.cpu`, `resources.limits.memory`).
- Compatible with k8s `nodeSelector`/`nodeAffinity`: a GPU-enabled sidecar container in the
  same pod can use the same GPU as the scoring container.

**Disadvantages:**

- Pod image is heavier: each node pod carries both Go scoring binary and Python + PyTorch
  sidecar. Estimated additional image size: ~1.5 GB for PyTorch CPU, ~5 GB for PyTorch CUDA.
- Sidecar lifecycle must be managed by the Operator (readiness probe, restart policy).
- Resource contention: if both containers compete for the same GPU EP, one must yield.
  Mitigation: sidecar trains on CPU or a separate inference EP; scoring uses the GPU.

**Resource estimates (per pod):**

- Sidecar RAM: ~400-600 MB for PyTorch CPU training on small batches.
- Sidecar CPU: ~0.5 vCPU sustained, ~2 vCPU burst during forward/backward pass.
- Sidecar GPU (optional): 0 if CPU-only; 2-4 GB VRAM if CUDA EP is enabled for training.
- Sidecar startup time: ~10-15 s (PyTorch import + Lightning module instantiation).

### Option B — Dedicated Training Nodes

A separate `vmafx-training-node` pool receives batched triples from the controller via gRPC.
Training nodes pull work from a shared queue, train, and push checkpoints.

**Advantages:**

- Clean separation of concerns: scoring and training resource pools are independently scaled.
- Training nodes can have larger GPU VRAM budgets (A100-class) without over-provisioning
  every scoring node.
- Simpler scoring pod images (no Python).

**Disadvantages:**

- Cross-pod data transport adds latency (1-10 s round-trip through the controller).
- Two new node types in the Helm chart and Operator: `VmafxNode` and `VmafxTrainingNode`.
- Training queue must be fault-tolerant: if training nodes are unavailable, triples must be
  buffered. Additional infrastructure (queue, storage).
- More complex scheduling: controller must track which training nodes are available and route
  triples accordingly.
- Overkill at v1 scale. For a few hundred to a few thousand triples per hour, a sidecar
  handles the load trivially.

### Option C — Go-Native Online Learning

Pure-Go SGD using Gorgonia or hand-rolled tensor math. No Python sidecar.

**Advantages:**

- Single Go binary. No Python dependency anywhere in the node image.
- Lowest per-sample overhead: no cross-process communication.

**Disadvantages:**

- Go ML ecosystem is not production-ready for full neural network training. Gorgonia is
  effectively unmaintained (last meaningful commit 2023). Hand-rolled SGD can handle linear
  heads but not multi-layer networks with normalization.
- LoRA-style adapter fine-tuning (the most viable use case) still requires a pre-computed
  frozen base, which must be loaded from the ONNX checkpoint. Gorgonia cannot load ONNX
  format natively; a custom parser would be required.
- The existing `ai/` PyTorch stack cannot be reused. Any model architecture changes would
  require parallel Go implementations.
- Path to future improvements (attention mechanisms, larger backbones) is blocked.

### Decision: Option A

Option A is recommended for v1. It reuses the complete existing `ai/` training infrastructure,
achieves sub-100 ms data-to-training latency, and isolates training failures from scoring.
Option B is deferred to v2 when cluster scale demands dedicated training GPU pools. Option C
is not viable for full neural network training at any scale.

---

## 3. Data Flow Design

### 3.1 Triple Capture in vmafx-node

**Where:** After `vmaf_read_pictures()` completes and `vmaf_score_pooled()` returns a
`predicted_score` for the job, before the result is serialised and sent to the controller.
This is the earliest moment all three components of the triple are available in the same
goroutine: reference features, distorted features, and the predicted score.

In executor pseudocode (Go):

```text
result, err := scorer.ScoreJob(ctx, job)
if err != nil { ... }
// Triple is available here — capture before returning result.
triple := &TrainingTriple{
    JobID:         job.ID,
    RefFeatures:   scorer.LastRefFeatures(),
    DisFeatures:   scorer.LastDisFeatures(),
    PredictedScore: result.VMIAFScore,
    Metadata:      captureMetadata(job),
}
if t.sidecarConn != nil {
    go t.shipTriple(ctx, triple)  // non-blocking; ring buffer absorbs bursts
}
return result, nil
```

The capture path must not block the result-reporting path. A goroutine-dispatched ring buffer
absorbs burst spikes when the sidecar is temporarily unavailable (startup, checkpoint export).

**Metadata schema** (per triple):

| Field | Type | Description |
|---|---|---|
| `job_id` | string | Controller-assigned job UUID |
| `codec` | string | Encoder used (libx264, libx265, libsvtav1, hevc_nvenc, …) |
| `preset` | string | Encoder preset string (medium, slow, p4, etc.) |
| `target_bitrate_kbps` | int32 | Target bitrate for this encode |
| `achieved_bitrate_kbps` | int32 | Actual achieved bitrate post-encode |
| `width` | int32 | Frame width in pixels |
| `height` | int32 | Frame height in pixels |
| `frame_rate` | float32 | Frame rate (fps) |
| `frame_count` | int32 | Number of frames scored |
| `color_space` | string | YUV420 / YUV420_10LE / P010 / etc. |
| `content_category` | string | Label from job tags (sports, animation, film, UGC, etc.) |
| `vmaf_model` | string | Model name used for scoring (vmaf_v0.6.1.json, chug-hdr-v1, etc.) |
| `backend` | string | Backend used (cpu, cuda, sycl, hip, vulkan) |
| `node_id` | string | vmafx-node pod name |
| `tenant_id` | string | Tenant / namespace identifier |
| `captured_at` | timestamp | Wall-clock time of capture (RFC 3339) |
| `ground_truth_score` | float32 | Optional: subjective MOS if available; NaN if not |

**Ring buffer:** An in-process bounded FIFO (configurable capacity, default 1000 triples)
absorbs rate spikes. If the buffer is full when a new triple arrives, the oldest entry is
dropped (oldest-eviction, not back-pressure). A Prometheus counter `vmafx_training_triples_dropped_total`
tracks evictions. The buffer is flushed to the sidecar whenever at least `min_flush_batch`
triples are queued (default 32) or a flush interval elapses (default 5 s), whichever comes first.

### 3.2 Transport: Go Node to Python Sidecar

**Chosen: gRPC over loopback (Option B in the transport evaluation).**

Rationale:

- The controller already uses gRPC for node communication; reusing the same framework avoids
  a second RPC library dependency.
- Proto definitions give strong typing and schema evolution via field numbering.
- gRPC streaming allows the Go node to open a single long-lived `TrainingTriple` stream to
  the sidecar rather than one RPC per triple.
- Backpressure: gRPC flow control naturally back-pressures the ring buffer flush.
- The sidecar exposes a single gRPC service (`VmafxSidecarTrainer`) on `localhost:50052`
  (non-overlapping with the node's own listener on `0.0.0.0:50051`).

Proto surface (sketch):

```protobuf
service VmafxSidecarTrainer {
  rpc StreamTriples(stream TrainingTriple) returns (TrainingAck);
  rpc GetStatus(StatusRequest) returns (SidecarStatus);
}

message TrainingTriple {
  string job_id = 1;
  repeated float ref_features = 2;     // ordered by feature extractor index
  repeated float dis_features = 3;
  float predicted_score = 4;
  TrainingMetadata metadata = 5;
}

message SidecarStatus {
  int64 samples_received = 1;
  int64 samples_trained = 2;
  string last_checkpoint_version = 3;
  google.protobuf.Timestamp last_checkpoint_at = 4;
}
```

Unix domain socket (`/tmp/vmafx-sidecar.sock`) is an alternative if loopback adds measurable
overhead; benchmarking should decide. For v1, TCP loopback is simpler to configure via k8s
container port and avoids FUSE/volume-mount dependencies for the socket path.

**Discarded transport options:**

- *Unix socket + msgpack:* lower overhead per message but requires a custom framing
  implementation and loses type safety. Not worth the maintenance cost.
- *Shared volume + file watch:* worst latency (seconds for inotify + file read), not suitable
  for near-real-time training.

### 3.3 Training Loop in the Python Sidecar

**Algorithm: Online SGD with exponential moving average (EMA) weight update.**

The sidecar maintains two model copies:

1. `live_model` — the model being trained in-place via SGD.
2. `ema_model` — an exponential moving average of `live_model` weights, updated after each
   gradient step with a configurable decay `beta` (default 0.999).

The EMA model is the one exported to ONNX. EMA is standard practice for online learning
because it smooths out noisy gradient updates and produces more stable checkpoints. The
`live_model` may temporarily degrade after a noisy batch; the EMA model lags slightly and
averages out the noise.

**Training loop pseudocode (Lightning-style):**

```python
class SidecarTrainer(pl.LightningModule):
    def __init__(self, base_model_path: str, lr: float = 1e-4, beta: float = 0.999):
        super().__init__()
        self.model = load_onnx_as_torch(base_model_path)
        self.ema_model = copy.deepcopy(self.model)
        self.lr = lr
        self.beta = beta
        self.sample_count = 0
        self.last_checkpoint_at = None

    def training_step(self, batch, batch_idx):
        ref_feats, dis_feats, targets = batch
        preds = self.model(ref_feats, dis_feats)
        loss = F.mse_loss(preds, targets)
        return loss

    def on_after_backward(self):
        # EMA weight update after each gradient step
        for p_live, p_ema in zip(self.model.parameters(), self.ema_model.parameters()):
            p_ema.data.mul_(self.beta).add_(p_live.data, alpha=1.0 - self.beta)
        self.sample_count += self._current_batch_size
        self._maybe_checkpoint()

    def _maybe_checkpoint(self):
        now = time.monotonic()
        time_ok = (now - self._last_ckpt_time) >= self.checkpoint_interval_s
        count_ok = (self.sample_count - self._last_ckpt_sample) >= self.min_samples
        if time_ok and count_ok:
            self._export_checkpoint()
            self._last_ckpt_time = now
            self._last_ckpt_sample = self.sample_count
```

**Checkpoint trigger:** dual-condition AND gate:

- Time since last checkpoint >= `checkpoint_interval` (default 10 minutes).
- New samples since last checkpoint >= `min_samples` (default 1000).

Both conditions must be met. This prevents exporting a model trained on only 5 triples
(useless) and prevents exporting the same model every 10 minutes when traffic is idle.

**Checkpoint format:** ONNX, exported via `torch.onnx.export()`. This is the same format
the rest of the VMAFX pipeline consumes (`onnxruntime-go`, libvmaf DNN path). No new
checkpoint format is introduced.

**Catastrophic forgetting mitigation — replay buffer:**

The sidecar maintains an in-memory replay buffer (configurable capacity, default 10,000
triples; FIFO eviction). Each training batch is a mix of new triples and a random sample from
the replay buffer (default ratio: 50% new, 50% replay). This prevents the model from
catastrophically forgetting its base distribution when hit with a burst of narrow-content
triples (e.g., a sudden wave of HDR animation encoding jobs that dominate the training stream
for an hour).

The replay buffer can optionally be persisted to a volume-mounted Parquet file for audit and
GDPR-compliant deletion. See Section 4 (Open Questions) for the persistence design.

**Base model initialization:**

The sidecar loads a base ONNX model from the model registry at startup (path configurable via
`VMAFX_BASE_MODEL_PATH` env var, defaulting to the `baseModel` field of the
`VmafxModelTraining` CRD). If the registry is unavailable at startup, the sidecar enters a
`waiting` state and retries with exponential backoff. The Go node's readiness probe checks the
sidecar's `GetStatus` RPC and withholds the `Ready` condition until the sidecar reports
`state=training`.

### 3.4 Checkpoint Pickup by Nodes

**Checkpoint storage:**

The sidecar writes checkpoints to a PVC mount at a configurable path (default
`/mnt/vmafx-models/<model_training_name>/<version>/model.onnx`). The version string is
derived from the base model version + an incrementing counter:
`<base>.<checkpoint_count>` (e.g., `chug-hdr-v1.42`).

Each checkpoint write is atomic: the sidecar writes to a `.tmp` suffix and renames
(`os.replace()`) to the final path. This prevents nodes from loading a partially-written
checkpoint.

A SHA-256 digest sidecar file (`model.onnx.sha256`) is written alongside the checkpoint.
Nodes verify the digest before loading.

**Node polling:**

Nodes poll the model registry at a configurable interval (default 60 s). The check is a
lightweight HEAD request to the registry (or a file-stat on the PVC mount). If the version
string has advanced, the node initiates a checkpoint swap.

**Atomic swap:**

1. Node downloads (or mmap-reads) the new checkpoint.
2. Node verifies the SHA-256 digest.
3. Node loads the new ONNX session into a second `onnxruntime-go` session object.
4. Node drains in-flight scoring jobs that reference the old session (wait for the current
   scoring goroutines to finish, bounded by `swap_drain_timeout`, default 30 s).
5. Node atomically swaps the session pointer (Go `atomic.Pointer[ort.Session]`).
6. Node closes the old session.

A Prometheus counter `vmafx_model_checkpoint_swaps_total` and gauge
`vmafx_model_checkpoint_version` track swap events. A rollback is triggered automatically if
the new checkpoint causes a `vmaf_score_pooled` failure rate > `rollback_threshold` (default
5% over a 5-minute window).

**Controller version selection:**

When the controller dispatches a new job, it includes the model version to use in the job
spec (`job.ModelVersion`). The node uses the specified version if it has it loaded; otherwise
it downloads it before starting the job. Version selection policy (configurable at the
`VmafxModelTraining` level):

| Policy | Behaviour |
|---|---|
| `latest` | Controller always uses the most recently committed checkpoint version. |
| `latest-stable` | Controller uses the most recently committed checkpoint that passed the PLCC gate (see stability gate below). |
| `pinned:<version>` | Controller always uses the specified version regardless of newer checkpoints. |

The **stability gate** runs as a lightweight background job in the controller: after each new
checkpoint is committed to the registry, the controller scores the VMAFX stability fixture set
(a small internal set of 10-20 reference/distorted pairs with known VMAF scores) and checks
that PLCC against the previous checkpoint's PLCC does not regress by more than
`stability_plcc_delta` (default 0.005). If it regresses, the checkpoint is tagged `unstable`
and excluded from `latest-stable` selection.

---

## 4. Operator Integration: VmafxModelTraining CRD

### 4.1 CRD Definition

```yaml
apiVersion: vmafx.dev/v1
kind: VmafxModelTraining
metadata:
  name: chug-hdr-v2-online-refine
  namespace: vmafx-production
spec:
  # The ONNX model to use as the starting point for online fine-tuning.
  # Must exist in the model registry at startup.
  baseModel: chug-hdr-v1

  # Which vmafx-node pods feed training triples to this training session.
  # The sidecar container is co-located with nodes whose labels match this selector.
  dataSource:
    selector:
      matchLabels:
        workload: chug-hdr
    # Optional: filter to triples with a minimum predicted score confidence.
    # Triples where |predicted_score - ema_score| > scoreDeviationThreshold are excluded.
    scoreDeviationThreshold: 5.0

  # Online learning algorithm configuration.
  algorithm: online-sgd
  algorithmConfig:
    learningRate: 0.0001
    emaDecay: 0.999
    batchSize: 32
    replayBufferSize: 10000
    replayMixRatio: 0.5  # fraction of each batch drawn from replay buffer

  # Checkpoint emission policy.
  checkpoint:
    # Both conditions must be satisfied before a checkpoint is emitted.
    interval: 10m
    minSamples: 1000
    # Stability gate: reject checkpoints that regress PLCC by more than this delta.
    stabilityPlccDelta: 0.005
    # Version selection policy for the controller: latest | latest-stable | pinned:<ver>
    versionPolicy: latest-stable

  # Where checkpoints are written.
  outputRegistry:
    type: pvc
    pvcName: vmafx-models
    path: /models/chug-hdr-v2-online-refine

  # Training data persistence (optional). If set, triples are written to Parquet for
  # audit and GDPR-compliant deletion. Disabled by default.
  dataPersistence:
    enabled: true
    storageClass: standard
    retentionDays: 90
    path: /runs/online-training-stream

  # Resource limits for the sidecar container injected into matching node pods.
  sidecarResources:
    requests:
      cpu: "0.5"
      memory: "512Mi"
    limits:
      cpu: "2"
      memory: "1Gi"

status:
  # Operator-managed status fields (not user-settable).
  state: training  # waiting | training | paused | error
  currentSamples: 12345
  lastCheckpoint: "2026-05-28T12:34:56Z"
  modelVersion: "chug-hdr-v1.42"
  activeNodeCount: 8  # number of node pods currently feeding this training session
  lastStabilityGateResult: passed  # passed | failed | skipped
  conditions:
    - type: Ready
      status: "True"
      reason: Training
      message: "8 nodes feeding; last checkpoint chug-hdr-v1.42 (2026-05-28T12:34:56Z)"
    - type: StabilityGatePassed
      status: "True"
      reason: PLCCWithinThreshold
      message: "PLCC delta 0.0002 < threshold 0.005"
```

### 4.2 Operator Reconcile Loop

The `vmafx-operator` reconciles `VmafxModelTraining` objects. High-level pseudocode:

```text
func (r *VmafxModelTrainingReconciler) Reconcile(ctx, req) (ctrl.Result, error):

    training = fetch VmafxModelTraining by req.NamespacedName
    if not found: return (no requeue)

    // 1. Find all vmafx-node Pods matching spec.dataSource.selector.
    matchingPods = listPods(labelSelector: training.spec.dataSource.selector)

    // 2. For each matching pod, ensure a sidecar container is injected.
    //    The sidecar is added via a MutatingWebhook (preferred) or via pod template
    //    patching on the VmafxNode's pod template. The reconciler verifies the
    //    sidecar container is present; if absent (e.g., pod predates this training
    //    object), it patches the VmafxNode's podTemplate and allows the rollout to
    //    happen naturally.
    for pod in matchingPods:
        if sidecarContainerAbsent(pod, training.Name):
            patchVmafxNodePodTemplate(pod.OwnerReference, training)

    // 3. Ensure the base model is available in the registry.
    if not registryHasModel(training.spec.baseModel):
        updateStatus(state=waiting, message="Base model not found in registry")
        return (requeue after 30s)

    // 4. Verify the sidecar's gRPC endpoint is reachable on at least one matching pod.
    reachableCount = 0
    for pod in matchingPods:
        if sidecarReachable(pod, grpcPort=50052):
            reachableCount++
    updateStatus(activeNodeCount=reachableCount)
    if reachableCount == 0:
        updateStatus(state=waiting, message="No reachable sidecar endpoints")
        return (requeue after 10s)
    updateStatus(state=training)

    // 5. Poll latest checkpoint status from the sidecar of any reachable pod.
    //    (All sidecars for the same VmafxModelTraining share a registry; any one
    //    reflects the current state.)
    status = sidecarGetStatus(matchingPods[0], grpcPort=50052)
    updateStatus(
        currentSamples=status.samples_trained,
        lastCheckpoint=status.last_checkpoint_at,
        modelVersion=status.last_checkpoint_version,
    )

    // 6. Run stability gate if a new checkpoint has been registered since the last
    //    reconcile cycle and the previous gate result was for an older version.
    if newCheckpointAvailable(training):
        gateResult = runStabilityGate(training.spec.checkpoint.stabilityPlccDelta,
                                       newCheckpointPath, fixtureSetPath)
        if gateResult.passed:
            markCheckpointStable(newCheckpointPath)
            updateCondition(StabilityGatePassed, True)
        else:
            markCheckpointUnstable(newCheckpointPath)
            updateCondition(StabilityGatePassed, False,
                            message="PLCC delta " + gateResult.delta + " exceeds threshold")

    // 7. Update status conditions and requeue after the checkpoint interval.
    updateCondition(Ready, True)
    return (requeue after training.spec.checkpoint.interval)
```

**MutatingWebhook for sidecar injection:** The preferred mechanism for sidecar injection is a
`MutatingAdmissionWebhook` that the operator registers. When a new `vmafx-node` pod is created
with labels that match an active `VmafxModelTraining`'s `dataSource.selector`, the webhook
injects the sidecar container spec (image, env vars, resource limits, gRPC port) automatically.
This avoids a reconcile-loop race between pod creation and the operator noticing the new pod.

---

## 5. Open Questions and Design Decisions

### 5.1 Training Data Persistence

**Should triples be persisted?**

Yes, with opt-in per `VmafxModelTraining` object (`spec.dataPersistence.enabled`). Rationale:

- **Audit trail:** the ability to reproduce a trained checkpoint from its training stream is a
  basic reproducibility requirement.
- **Replay on demand:** if a new model architecture is introduced, the accumulated triple
  stream can be replayed to bootstrap the new model without re-running all the original
  encoding jobs.
- **GDPR / data governance:** content-provider data (encoded video features) may be subject
  to deletion obligations. Persisting triples to a structured format (Parquet) with a
  `job_id` and `tenant_id` field enables targeted deletion. A `VmafxDataDeletion` CRD or a
  controller endpoint can handle deletion requests.

**Format:** Parquet (columnar, compressed via Snappy). Written by the Python sidecar via
`pandas.DataFrame.to_parquet()` or `pyarrow.parquet.write_to_dataset()`. Files are partitioned
by date and model training name:
`/runs/online-training-stream-<name>/<YYYY-MM-DD>/part-<N>.parquet`.

**Retention:** configurable `spec.dataPersistence.retentionDays` (default 90 days). The
operator runs a nightly cleanup job that deletes Parquet partitions older than the retention
window.

### 5.2 Controller Version Selection Policy

See Section 3.4 above. The three policies (`latest`, `latest-stable`, `pinned:<version>`) are
declared per `VmafxModelTraining` object. The controller reads the policy from the training
object's status when dispatching a job. If the job's tenant has a tenant-level override (not
yet designed in v1), the tenant-level policy takes precedence.

For v1, `latest-stable` is the default. This is the conservative choice: operators who want
to live on the bleeding edge can switch to `latest`.

### 5.3 Catastrophic Forgetting Mitigation

The replay buffer (Section 3.3) is the primary mitigation. Additional measures:

- **Elastic weight consolidation (EWC):** a regularization term that penalizes updates to
  weights that were important for previous tasks. EWC is deferred to v2; it requires computing
  the Fisher information matrix on the base training set, which is not available at sidecar
  startup. Research-track item.
- **Learning rate warmup:** when the sidecar first starts or after a long idle period, apply a
  linear learning rate warmup over the first 100 steps to prevent overwriting the base model
  weights with a noisy gradient step. Default warmup_steps=100.
- **Gradient clipping:** clip gradients to `max_grad_norm=1.0` to prevent pathological updates
  from outlier triples (e.g., a triple with a corrupt feature vector).

### 5.4 Below-Target Encoding Triples

Triples generated from encodings that fall below the VMAF target (e.g., a CRF sweep step that
is intentionally under-targeted) are still useful training data. They represent real distortion
patterns at real bitrates. The model should learn that low-bitrate distorted streams score
poorly — this is signal, not noise.

No filtering on below-target triples by default. An optional `spec.dataSource.scoreDeviationThreshold`
field can exclude triples where `|predicted_score - target_score| > threshold` if the operator
wants to exclude extreme outliers, but this is not recommended for the general case.

### 5.5 Multi-Tenant Isolation

In a multi-tenant deployment (multiple content providers sharing a VMAFX cluster), training
data from tenant A must not influence the model served to tenant B. Options:

- **Per-tenant `VmafxModelTraining` objects:** each tenant gets their own training object
  with a matching `dataSource.selector` (e.g., `matchLabels: { tenant: acme-streaming }`) and
  their own output registry path. Tenant data streams are fully isolated at the triple level.
- **Shared base, per-tenant adapters:** a shared global base model is continuously updated
  from all tenants' triples; per-tenant adapter heads (LoRA-style) are trained on
  tenant-specific data. This maximises data efficiency but requires model architecture support
  for conditional adapters. Deferred to v2.

For v1: per-tenant `VmafxModelTraining` objects. Simple, isolated, immediately implementable.

---

## 6. Integration Points with Existing ai/ Stack

The Python sidecar is not a new ML framework. It is a runtime wrapper around the existing
`ai/` training infrastructure:

| Component | Role in sidecar |
|---|---|
| `ai/src/vmaf_ai/models/` | Model architecture classes (`VmafRegressor`, `VmafNRProxy`, etc.) loaded via `importlib` in the sidecar. |
| `ai/scripts/train_fr_regressor.py` | Reference implementation for the training loop. The sidecar's `SidecarTrainer` is a stripped-down Lightning module that pulls from the same model classes. |
| `ai/scripts/export_tiny_models.py` | ONNX export logic. The sidecar's `_export_checkpoint()` method reuses the same `torch.onnx.export()` call with the same operator compatibility settings (opset 17, same as ADR-0249). |
| `model/tiny/*.onnx` | Base models loaded at sidecar startup. The sidecar continues from a pre-trained checkpoint; it does not train from random initialization. |
| `model/tiny/*.json` | Sidecar JSON metadata. The sidecar writes an updated sidecar JSON alongside each ONNX checkpoint with incremented version and updated training provenance fields. |

The `ai/scripts/` batch training path is not deprecated by the sidecar. It remains the
authoritative path for initial model training from the full Netflix + CHUG corpus. The sidecar
performs continuous refinement of an already-trained model, not from-scratch training.

---

## 7. Prior Art and References

- **Continual learning survey (Parisi et al., 2019):** comprehensive survey of catastrophic
  forgetting mitigations. EWC, Progressive Neural Networks, PackNet. Our replay buffer
  approach is a simplified Experience Replay (ER) variant.
- **EMA for model stability (Polyak averaging):** well-established in RL (target networks in
  DQN) and online learning (SGD with Polyak averaging). The `beta=0.999` default follows
  the Mean Teacher paper (Tarvainen & Valpola, 2017) and is standard in self-supervised
  video quality work.
- **k8s sidecar container pattern:** the k8s 1.29 "sidecar containers" feature (KEP-753) adds
  native init-container-like lifecycle semantics for sidecars; if the cluster is k8s 1.29+,
  the sidecar should be declared as a `spec.initContainers` entry with `restartPolicy:
  Always` to get proper lifecycle management.
- **LoRA (Hu et al., 2021):** low-rank adapter fine-tuning. Relevant for v2 per-tenant
  adapter heads. Not in scope for v1.
- **ADR-0709** — Phase 4b umbrella; item 4b.7 directly mandates this research digest.
- **ADR-0249** — ONNX export recipe and opset constraints; sidecar export must be compatible.
- **ADR-0042** — tiny-AI per-PR docs rule; sidecar training operator surfaces require docs
  under `docs/ai/` in the Phase 4b.7 implementation PR.

---

## 8. Implementation Checklist (for 4b.7 PR)

When the Phase 4b.7 implementation PR lands, the following must be shipped in the same PR:

- [ ] `cmd/vmafx-sidecar/` — Python sidecar container entry point and gRPC service
      implementation (`SidecarTrainerServicer`).
- [ ] `proto/vmafx/sidecar/v1/sidecar.proto` — `VmafxSidecarTrainer` service definition.
- [ ] `pkg/sidecar/` (Go) — ring buffer, gRPC client, sidecar health monitor in `vmafx-node`.
- [ ] Updated `deploy/helm/vmafx/templates/vmafxnode.yaml` — sidecar container spec,
      gRPC port, resource limits.
- [ ] Updated `deploy/helm/vmafx/crds/vmafxmodeltraining.yaml` — CRD definition.
- [ ] Operator reconciler: `internal/controller/vmafxmodeltraining_controller.go`.
- [ ] MutatingWebhook: `internal/webhook/sidecar_injector.go`.
- [ ] `docs/ai/sidecar-training.md` — operator guide (ADR-0042 five-point bar).
- [ ] `docs/architecture/sidecar-training-c4.md` — C4 context + container diagrams.
- [ ] `changelog.d/changed/sidecar-training-research.md` — this research digest fragment.
- [ ] `docs/state.md` row in Recently closed.

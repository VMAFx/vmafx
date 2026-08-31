# vmafx-operator

The vmafx-operator is a Kubernetes Operator built with kubebuilder v4 /
controller-runtime v0.19+ that manages the three VMAFX custom resource types:

| CRD | Short name | Purpose |
| --- | --- | --- |
| `VmafxJob` | `vmjob` | One reference↔distorted video-quality scoring job |
| `VmafxNode` | `vmnode` | A compute node with GPU capacity |
| `VmafxModelTraining` | `vmtrain` | Online SGD-EMA sidecar model training run |

See [ADR-0714](../adr/0714-vmafx-operator-skeleton.md) for the design decision
and [ADR-0709](../adr/0709-vmafx-phase4b-distributed-platform.md) for the
broader Phase 4b context.

---

## Quick start

### Install CRDs + operator via Helm

```bash
# Clone and checkout the branch.
git clone https://github.com/VMAFx/vmafx.git && cd vmafx

# Install CRDs + operator (Stage 1 — stub reconcilers).
helm upgrade --install vmafx deploy/helm/vmafx \
  --set operator.enabled=true \
  --set operator.image.tag=latest \
  --namespace vmafx-system --create-namespace
```

CRDs are installed automatically from `deploy/helm/vmafx/crds/` on first
`helm install`.

### Verify the operator image version

The release image exposes a non-blocking version check that does not need
Kubernetes credentials or start the manager:

```bash
docker run --rm ghcr.io/vmafx/vmafx-operator:v3.2.1 --version
# v3.2.1
```

Release builds inject the published tag into `pkg/version.version`; an output
of `dev` means the image was not built by the release workflow.

### Submit a scoring job

```yaml
# job.yaml
apiVersion: vmafx.dev/v1
kind: VmafxJob
metadata:
  name: my-score-job
  namespace: vmafx-system
spec:
  reference:  "s3://my-bucket/ref.yuv"
  distorted:  "s3://my-bucket/dist.yuv"
  model:      "vmaf_v0.6.1"
  backend:    "cuda"
  priority:   10
```

```bash
kubectl apply -f job.yaml
kubectl get vmjob -n vmafx-system
# NAME            PHASE     SCORE   NODE   AGE
# my-score-job    Pending   <none>  <none> 3s
```

### Register a compute node

```yaml
# node.yaml
apiVersion: vmafx.dev/v1
kind: VmafxNode
metadata:
  name: gpu-node-0
  namespace: vmafx-system
spec:
  gpuVendor: nvidia
  capacity: 4
  image: ghcr.io/vmafx/vmafx-node:latest
```

```bash
kubectl apply -f node.yaml
kubectl get vmnode -n vmafx-system
# NAME         VENDOR   HEALTHY   JOBS   DEVICE   AGE
# gpu-node-0   nvidia   true      0               12s
```

### Start a training run

```yaml
# training.yaml
apiVersion: vmafx.dev/v1
kind: VmafxModelTraining
metadata:
  name: online-training-1
  namespace: vmafx-system
spec:
  baseModel:      "vmaf_v0.6.1"
  algorithm:      "online-sgd-ema"
  outputRegistry: "ghcr.io/vmafx/models"
  dataSource:
    nodeSelector:
      gpu.vendor: nvidia
  checkpoint:
    interval:   "10m"
    minSamples: 1000
```

```bash
kubectl apply -f training.yaml
kubectl get vmtrain -n vmafx-system
# NAME               PHASE          SAMPLES   MODELVERSION   AGE
# online-training-1  Initializing   0         <none>         5s
```

---

## Architecture

The operator runs as a single Deployment (`vmafx-operator`) with a
controller-runtime Manager.  Three independent reconcilers watch their
respective CRDs.

```text
┌─────────────────────────────────────────────┐
│              vmafx-operator Pod             │
│                                             │
│  ┌───────────────────────────────────────┐  │
│  │  controller-runtime Manager           │  │
│  │  ┌─────────────────────────────────┐  │  │
│  │  │ VmafxJobReconciler              │  │  │
│  │  │  • Polls GetJob gRPC every 10 s │  │  │
│  │  │  • Maps PENDING/RUNNING/        │  │  │
│  │  │    COMPLETED/FAILED → CR phase  │  │  │
│  │  │  • Writes Score on Succeeded    │  │  │
│  │  ├─────────────────────────────────┤  │  │
│  │  │ VmafxNodeReconciler             │  │  │
│  │  │  • /healthz probe every 30 s    │  │  │
│  │  │  • 60 s stale-heartbeat gate    │  │  │
│  │  │  • Updates Healthy + heartbeat  │  │  │
│  │  ├─────────────────────────────────┤  │  │
│  │  │ VmafxModelTrainingReconciler    │  │  │
│  │  │  • Polls sidecar /status 60 s   │  │  │
│  │  │  • Emits CheckpointWritten event│  │  │
│  │  └─────────────────────────────────┘  │  │
│  └───────────────────────────────────────┘  │
│  Prometheus metrics :8081  │  Healthz :8082  │
└─────────────────────────────────────────────┘
```

---

## Helm values reference (`operator.*`)

| Key | Default | Description |
| --- | --- | --- |
| `operator.enabled` | `false` | Deploy the operator Deployment + RBAC |
| `operator.replicaCount` | `1` | Number of operator Pods |
| `operator.image.repository` | `ghcr.io/vmafx/vmafx-operator` | Image repository |
| `operator.image.tag` | `""` (→ Chart.AppVersion) | Image tag |
| `operator.image.pullPolicy` | `IfNotPresent` | Pull policy |
| `operator.logLevel` | `info` | Log level: debug \| info \| warn \| error |
| `operator.leaderElect` | `false` | Enable leader election (requires ≥2 replicas) |
| `operator.resources` | see values.yaml | CPU/memory limits + requests |

---

## Environment variables

As of ADR-1119 Phase 1 the operator is composed with the golusoris fx
framework and is configured purely through environment variables (the previous
CLI flags are removed; fx owns signals and the run loop). Config is read from
the `operator.*` koanf subtree under the `VMAFX_` prefix.

| Variable | Default | Description |
| --- | --- | --- |
| `VMAFX_OPERATOR_METRICS_ADDR` | `:8081` | Prometheus metrics endpoint (`0` disables) |
| `VMAFX_OPERATOR_HEALTH_PROBE_ADDR` | `:8082` | Health probe endpoint |
| `VMAFX_OPERATOR_LEADER_ELECTION` | `false` | Enable leader election |
| `VMAFX_OPERATOR_LEADER_ELECTION_ID` | `vmafx-operator.vmafx.dev` | Lease name used when leader election is enabled |
| `VMAFX_OPERATOR_WEBHOOK_PORT` | `0` | Admission-webhook port; `0` disables webhooks |
| `VMAFX_OPERATOR_WEBHOOK_HOST` | _(all interfaces)_ | Admission-webhook bind host |
| `VMAFX_OPERATOR_GRACEFUL_SHUTDOWN` | `30s` | Manager graceful-shutdown timeout |
| `VMAFX_LOG_LEVEL` | `info` | Log verbosity (golusoris log module: `debug\|info\|warn\|error`) |
| `VMAFX_CONTROLLER_GRPC_ADDR` | `vmafx-controller.<ns>.svc.cluster.local:9090` | gRPC address of the vmafx-controller |
| `VMAFX_CONTROLLER_HTTP_ADDR` | `http://vmafx-controller.<ns>.svc.cluster.local:8080` | HTTP address of the vmafx-controller |

> **Migration from the pre-fx binary** (ADR-1119): the CLI flags
> (`--metrics-bind-address`, `--health-probe-bind-address`, `--leader-elect`,
> `--log-level`, `--webhooks-enabled`) are removed. Three env vars were
> renamed — `VMAFX_OPERATOR_PROBE_ADDR` → `VMAFX_OPERATOR_HEALTH_PROBE_ADDR`,
> `VMAFX_OPERATOR_LEADER_ELECT` → `VMAFX_OPERATOR_LEADER_ELECTION`,
> `VMAFX_OPERATOR_LOG_LEVEL` → `VMAFX_LOG_LEVEL` — and the boolean
> `VMAFX_OPERATOR_WEBHOOKS_ENABLED` is replaced by the integer
> `VMAFX_OPERATOR_WEBHOOK_PORT` (set a port such as `9443` to enable; `0` or
> unset disables). Update Deployment manifests and Helm values accordingly.

---

## Running tests

### Controller envtest suite

The envtest suite installs the CRDs into an embedded etcd + API server and
verifies each reconciler's Stage 2 behaviour (7 specs).

```bash
# Install envtest binaries (one-time).
go install sigs.k8s.io/controller-runtime/tools/setup-envtest@latest
export KUBEBUILDER_ASSETS=$(setup-envtest use 1.31 -p path)

# Run the controller suite.
go test ./cmd/vmafx-operator/internal/controller/... -v
```

### Webhook unit tests

Webhook validators are pure Go functions — no API server needed.

```bash
go test ./cmd/vmafx-operator/internal/webhook/... -v
```

### Run all operator tests

```bash
export KUBEBUILDER_ASSETS=$(setup-envtest use 1.31 -p path)
go test ./cmd/vmafx-operator/... -v
```

---

## Webhook admission validation

Webhooks are disabled by default.  Enable by setting a webhook port, e.g.
`VMAFX_OPERATOR_WEBHOOK_PORT=9443` (and optionally
`VMAFX_OPERATOR_WEBHOOK_HOST`).  When enabled, the operator validates:

| CRD | Field | Rule |
| --- | --- | --- |
| `VmafxJob` | `spec.reference`, `spec.distorted` | Must be a valid rclone URI: non-empty, `scheme://` form |
| `VmafxNode` | `spec.gpuVendor` | Must be one of `nvidia`, `amd`, `intel`, `cpu` |

Valid URI schemes include `file://`, `s3://`, `rclone://`, `gs://`, `azure://`,
and any other `alphabet://` URI supported by rclone.

**TLS prerequisite**: the webhook server requires a valid TLS certificate.  Install
[cert-manager](https://cert-manager.io/) and annotate the webhook `Service` with
`cert-manager.io/inject-ca-from` to auto-provision the certificate.

---

## RBAC

Three minimum-permission `ClusterRole` manifests are provided:

| File | Controller | Key verbs |
| --- | --- | --- |
| `config/rbac/role_vmafxjob.yaml` | VmafxJob | get/list/watch/update/patch vmafxjobs + status |
| `config/rbac/role_vmafxnode.yaml` | VmafxNode | get/list/watch/update/patch vmafxnodes + status |
| `config/rbac/role_vmafxmodeltraining.yaml` | VmafxModelTraining | get/list/watch/update/patch vmafxmodeltrainings + status |

All three roles include `events: create/patch` (for event emission) and
`leases: *` (for leader election).  The `config/rbac/role.yaml` is the combined
aggregate used by the Helm operator RBAC template.

---

## Stage roadmap

| Stage | Status | Scope |
| --- | --- | --- |
| Stage 1 | Shipped (ADR-0714) | Skeleton, CRDs, stub reconcilers, Helm integration, envtest |
| Stage 2 | Shipped (ADR-0786) | gRPC poll loop, stale-heartbeat gate, checkpoint events, webhook validation, per-controller RBAC |
| Stage 3 | Planned | VmafxJob Pod lifecycle (create/watch/delete), controller-gen codegen CI job |
| Stage 4 | Planned | VmafxModelTraining SGD-EMA controller, checkpoint OCI push |

---

## Related documents

- [ADR-0714](../adr/0714-vmafx-operator-skeleton.md) — Stage 1 design
- [ADR-0786](../adr/0786-vmafx-operator-stage2-reconcilers.md) — Stage 2 design
- [ADR-0709](../adr/0709-vmafx-phase4b-distributed-platform.md) — Phase 4b platform
- [ADR-0711](../adr/0711-vmafx-controller-impl.md) — controller (sibling service)
- [k8s-deployment.md](k8s-deployment.md) — general k8s deployment guide
- [gpu-scheduling.md](gpu-scheduling.md) — GPU vendor scheduling

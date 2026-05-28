# vmafx-operator

The vmafx-operator is a Kubernetes Operator built with kubebuilder v4 /
controller-runtime v0.19+ that manages the three VMAFX custom resource types:

| CRD | Short name | Purpose |
|---|---|---|
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

```
┌─────────────────────────────────────────────┐
│              vmafx-operator Pod             │
│                                             │
│  ┌───────────────────────────────────────┐  │
│  │  controller-runtime Manager           │  │
│  │  ┌─────────────────────────────────┐  │  │
│  │  │ VmafxJobReconciler              │  │  │
│  │  │  • Pending → Running on assign  │  │  │
│  │  │  • Stage 2: Pod lifecycle       │  │  │
│  │  ├─────────────────────────────────┤  │  │
│  │  │ VmafxNodeReconciler             │  │  │
│  │  │  • /healthz probe every 30 s    │  │  │
│  │  │  • Updates Healthy + heartbeat  │  │  │
│  │  ├─────────────────────────────────┤  │  │
│  │  │ VmafxModelTrainingReconciler    │  │  │
│  │  │  • Initializing phase on create │  │  │
│  │  │  • Stage 2: SGD-EMA loop        │  │  │
│  │  └─────────────────────────────────┘  │  │
│  └───────────────────────────────────────┘  │
│  Prometheus metrics :8081  │  Healthz :8082  │
└─────────────────────────────────────────────┘
```

---

## Helm values reference (`operator.*`)

| Key | Default | Description |
|---|---|---|
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

| Variable | CLI flag equivalent | Default | Description |
|---|---|---|---|
| `VMAFX_OPERATOR_METRICS_ADDR` | `--metrics-bind-address` | `:8081` | Prometheus metrics endpoint |
| `VMAFX_OPERATOR_PROBE_ADDR` | `--health-probe-bind-address` | `:8082` | Health probe endpoint |
| `VMAFX_OPERATOR_LEADER_ELECT` | `--leader-elect` | `false` | Enable leader election |
| `VMAFX_OPERATOR_LOG_LEVEL` | `--log-level` | `info` | Log verbosity |

---

## Running tests

The envtest suite installs the CRDs into an embedded etcd + API server and
verifies each reconciler's Stage 1 behaviour.

```bash
# Install envtest binaries (one-time).
go install sigs.k8s.io/controller-runtime/tools/setup-envtest@latest
export KUBEBUILDER_ASSETS=$(setup-envtest use 1.31 -p path)

# Run the suite.
go test ./cmd/vmafx-operator/internal/controller/...
```

---

## Stage roadmap

| Stage | Status | Scope |
|---|---|---|
| Stage 1 (this PR) | Shipped | Skeleton, CRDs, stub reconcilers, Helm integration, envtest |
| Stage 2 | Planned | VmafxJob Pod lifecycle (create/watch/delete), score propagation, rolling upgrades |
| Stage 2 | Planned | VmafxModelTraining SGD-EMA controller, checkpoint push |
| Stage 3 | Planned | controller-gen codegen CI job, webhook admission validation |

---

## Related documents

- [ADR-0714](../adr/0714-vmafx-operator-skeleton.md) — operator design decision
- [ADR-0709](../adr/0709-vmafx-phase4b-distributed-platform.md) — Phase 4b platform
- [ADR-0711](../adr/0711-vmafx-controller-impl.md) — controller (sibling service)
- [k8s-deployment.md](k8s-deployment.md) — general k8s deployment guide
- [gpu-scheduling.md](gpu-scheduling.md) — GPU vendor scheduling

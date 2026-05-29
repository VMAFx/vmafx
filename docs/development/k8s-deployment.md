# Kubernetes Deployment (Helm)

VMAFX ships a Helm chart under `deploy/helm/vmafx/` that supports three
workload types and all three GPU device-plugin vendors (NVIDIA, AMD, Intel).

## Prerequisites

- Helm v3.12 or later
- A Kubernetes cluster (1.26+) with at least one GPU node (or CPU-only for
  testing)
- The relevant GPU device-plugin daemonset installed on GPU nodes — see
  [GPU scheduling guide](gpu-scheduling.md)

## Quick start

```bash
# Add chart dependencies (prometheus-pushgateway — optional)
helm dependency build deploy/helm/vmafx/

# Install with NVIDIA GPU (default)
helm upgrade --install vmafx deploy/helm/vmafx/ \
  --namespace vmafx --create-namespace

# Install CPU-only (no GPU required)
helm upgrade --install vmafx deploy/helm/vmafx/ \
  --namespace vmafx --create-namespace \
  --set gpu.enabled=false \
  --set gpu.vendor=cpu

# Install with AMD GPU (HIP backend)
helm upgrade --install vmafx deploy/helm/vmafx/ \
  --namespace vmafx --create-namespace \
  --set gpu.vendor=amd

# Install with Intel GPU (SYCL backend)
helm upgrade --install vmafx deploy/helm/vmafx/ \
  --namespace vmafx --create-namespace \
  --set gpu.vendor=intel
```

## GPU vendor matrix

| `gpu.vendor` | Kubernetes resource | VMAFX backend | Required device-plugin |
|---|---|---|---|
| `nvidia` | `nvidia.com/gpu` | `cuda` | [NVIDIA device plugin](https://github.com/NVIDIA/k8s-device-plugin) |
| `amd` | `amd.com/gpu` | `hip` | [AMD ROCm device plugin](https://github.com/RadeonOpenCompute/k8s-device-plugin) |
| `intel` | `gpu.intel.com/i915` | `sycl` | [Intel GPU plugin](https://github.com/intel/intel-device-plugins-for-kubernetes) |
| `cpu` | _(none)_ | `cpu` | _(none)_ |

The chart automatically sets the `VMAFX_BACKEND` environment variable inside
the container based on `gpu.vendor`, so the VMAFX runtime picks the correct
backend without further configuration.

**Vulkan note:** Vulkan is not a separate Kubernetes resource. It runs through
whichever GPU device-plugin is allocated. See
[GPU scheduling guide](gpu-scheduling.md#vulkan-and-kubernetes).

## Workload types

Select a workload type with `--set workload=<type>`.

### Deployment (default) — long-running HTTP scoring server

```bash
helm upgrade --install vmafx deploy/helm/vmafx/ \
  --set workload=Deployment \
  --set deployment.replicaCount=3
```

The server exposes:

- `GET /healthz` — liveness probe
- `GET /readyz` — readiness probe
- `GET /metrics` — Prometheus metrics (optional; enable `monitoring.enabled=true`)

### Job — one-shot batch scoring

Suitable for CI pipelines, nightly ladder runs, and `vmaf-tune compare` jobs.

```yaml
# batch-values.yaml
workload: Job
gpu:
  vendor: nvidia
  count: 1
job:
  command: ["vmaf-tune"]
  args: ["compare", "--config", "/corpus/batch.yaml"]
  ttlSecondsAfterFinished: 3600
```

```bash
helm upgrade --install vmafx-batch deploy/helm/vmafx/ \
  --namespace vmafx --create-namespace \
  --values batch-values.yaml
kubectl wait -n vmafx job/vmafx-batch --for=condition=complete --timeout=30m
```

### StatefulSet — MCP server with sticky session state

Used when the MCP server requires stable identity and persistent state (e.g.,
session caches, socket file).

```bash
helm upgrade --install vmafx-mcp deploy/helm/vmafx/ \
  --set workload=StatefulSet
```

Each pod gets a dedicated `1Gi` PVC at `/var/lib/vmafx`.

## Environment variable reference

| Variable | Set by | Description |
|---|---|---|
| `VMAFX_BACKEND` | Chart (from `gpu.vendor`) | Backend selector: `cuda`, `hip`, `sycl`, `cpu` |
| `VMAFX_MODEL_DIR` | ConfigMap (`config.VMAFX_MODEL_DIR`) | Path to VMAF model JSON files |
| `VMAFX_OUTPUT_DIR` | ConfigMap (`config.VMAFX_OUTPUT_DIR`) | Path for scored output |
| Any `VMAFX_*` | `values.yaml` `env:` block | Override arbitrary env vars |

To add extra variables:

```yaml
# values.yaml override
env:
  VMAFX_LOG_LEVEL: debug
  VMAFX_THREADS: "8"
```

## Persistence

All PVCs are opt-in:

```yaml
persistence:
  enabled: true
  storageClass: standard    # leave empty for default StorageClass
  corpus:
    enabled: true
    size: 100Gi
    mountPath: /corpus
  output:
    enabled: true
    size: 20Gi
    mountPath: /output
  models:
    enabled: true
    size: 2Gi
    mountPath: /models
```

## Scaling

```bash
# Horizontal scale (Deployment only)
kubectl scale -n vmafx deployment/vmafx --replicas=4

# Rolling update to a new image
kubectl set image -n vmafx deployment/vmafx \
  vmafx=ghcr.io/vmafx/vmafx:3.1.0
```

The chart uses `RollingUpdate` strategy with `maxUnavailable=0` by default,
ensuring zero-downtime updates.

## Monitoring

Enable Prometheus scraping via ServiceMonitor (requires
[prometheus-operator](https://github.com/prometheus-operator/prometheus-operator)):

```yaml
monitoring:
  enabled: true
  serviceMonitor:
    labels:
      release: prometheus    # match your Prometheus operator selector
    interval: 30s
```

For Job workloads that cannot expose a scrape endpoint, use the
Prometheus Pushgateway dependency:

```yaml
pushgateway:
  enabled: true
```

## Ingress

```yaml
ingress:
  enabled: true
  className: nginx
  annotations:
    cert-manager.io/cluster-issuer: letsencrypt-prod
  hosts:
    - host: vmafx.example.com
      paths:
        - path: /
          pathType: Prefix
  tls:
    - secretName: vmafx-tls
      hosts:
        - vmafx.example.com
```

## Common operations

### Check pod GPU allocation

```bash
kubectl describe pod -n vmafx -l app.kubernetes.io/name=vmafx \
  | grep -A 5 "Limits:"
```

### Port-forward for local testing

```bash
kubectl port-forward -n vmafx svc/vmafx 8080:8080
curl http://localhost:8080/healthz
```

### Run the built-in Helm test

```bash
helm test vmafx -n vmafx
```

### Uninstall

```bash
helm uninstall vmafx -n vmafx
# PVCs are NOT deleted automatically — remove explicitly if desired:
kubectl delete pvc -n vmafx -l app.kubernetes.io/instance=vmafx
```

## Security context

All pods run as non-root (`runAsUser: 65534`), with a read-only root
filesystem and all Linux capabilities dropped.  Adjust in `values.yaml`
under `podSecurityContext` and `securityContext` if your image requires
write access outside of explicitly mounted volumes.

## Related

- [GPU scheduling guide](gpu-scheduling.md)
- [Production Dockerfile](../../deploy/Dockerfile) — ADR-0698
- [Cloud-native redesign](../../docs/adr/0697-vmafx-cloud-native-redesign.md) — ADR-0697
- [Helm chart ADR](../../docs/adr/0699-vmafx-helm-chart-k8s.md) — ADR-0699

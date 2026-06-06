<!-- markdownlint-disable MD013 MD060 -->
# Kubernetes Deployment (Helm)

VMAFX ships a Helm chart under `deploy/helm/vmafx/` that supports three
workload types and all three GPU device-plugin vendors (NVIDIA, AMD, Intel).

A `values.schema.json` (Draft 2020-12) sits next to `values.yaml` and is
consulted automatically by `helm install`, `helm upgrade`, and `helm lint
--strict`. The schema enforces enum constraints on the load-bearing fields
(`workload`, `gpu.vendor`, `storage.mode`, `service.type`,
`image.pullPolicy`, `persistence.accessMode`, `operator.logLevel`,
`statefulSet.podManagementPolicy`,
`monitoring.serviceMonitor.scheme`) and uses `additionalProperties:
false` on every typed sub-object so sibling-key typos
(`replicaCounts`, `repostiory`, `maxSurg`) fail fast at install time
instead of silently rendering a broken manifest. See
[ADR-0870](../adr/0870-helm-values-schema-and-container-rebuild-audit.md)
for the rationale.

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

## Pod security {#pod-security}

Every pod the chart emits — controller `Deployment`, batch `Job`, sticky
`StatefulSet`, `vmafx-node` worker `Deployment`, and the `vmafx-operator`
`Deployment` — satisfies the Kubernetes [Pod Security Admission
"restricted"](https://kubernetes.io/docs/concepts/security/pod-security-admission/)
profile (ADR-0930):

| Setting                          | Value                              | Why                                                                                          |
|----------------------------------|------------------------------------|----------------------------------------------------------------------------------------------|
| `runAsNonRoot`                   | `true`                             | Required by `restricted`; matches the `USER nonroot:nonroot` directive in every production image (ADR-0878). |
| `runAsUser` / `runAsGroup`       | `65532`                            | Distroless `gcr.io/distroless/cc-debian12` baked-in nonroot UID/GID — keeps file ownership consistent across `emptyDir`, PVCs, and rclone caches. |
| `readOnlyRootFilesystem`         | `true`                             | Writes are restricted to explicitly-mounted `emptyDir` / PVC volumes (`/tmp`, the StatefulSet's `/var/lib/vmafx`).  Catches privilege-escalation primitives that depend on overwriting on-disk binaries. |
| `allowPrivilegeEscalation`       | `false`                            | Drops the `no_new_privs` exec bit; covers the SUID and `cap_setuid` escape paths.            |
| `capabilities.drop`              | `[ALL]`                            | Distroless containers do not need `CAP_NET_BIND_SERVICE` etc.; everything is dropped.        |
| `seccompProfile.type`            | `RuntimeDefault`                   | Engages the container-runtime default syscall filter (Docker/containerd ship a reasonable allow-list).  Required by `restricted` since k8s 1.25. |

To enforce the profile cluster-side, label your install namespace
([k8s docs](https://kubernetes.io/docs/concepts/security/pod-security-admission/#pod-security-admission-labels-for-namespaces)):

```bash
kubectl label --overwrite namespace vmafx-prod \
  pod-security.kubernetes.io/enforce=restricted \
  pod-security.kubernetes.io/audit=restricted \
  pod-security.kubernetes.io/warn=restricted
```

If your image requires write access outside the mounted volumes, override
`podSecurityContext` / `securityContext` in `values.yaml` — but doing so
moves the namespace out of the `restricted` profile.

## NetworkPolicy {#networkpolicy}

Disabled by default (`networkPolicy.enabled=false`) because many clusters
either ship their own CNI-managed policies (Cilium ClusterwideNetworkPolicy,
Calico GlobalNetworkPolicy) or do not install a NetworkPolicy controller —
in the latter case the chart's NetworkPolicies render but are inert.

Opt in with `--set networkPolicy.enabled=true`.  The chart then emits a
default-deny baseline plus four narrow allow-rules:

| Policy                          | Direction | Peer                                            | Ports               | Purpose                                              |
|---------------------------------|-----------|-------------------------------------------------|---------------------|------------------------------------------------------|
| `default-deny`                  | both      | _(no allow)_                                    | _(all)_             | Safety net — drops everything that is not explicitly allowed. Emitted per workload component (root / operator / node) so a new component without an allow-rule remains isolated. |
| `allow-http-ingress`            | ingress   | every pod in the release namespace              | `service.targetPort`| Scoring server reachable from any in-namespace client. |
| `allow-controller-to-node`      | ingress   | controller pods (selector match)                | `50051` (configurable) | gRPC dispatch from controller to `vmafx-node` workers. |
| `allow-node-egress-object-store`| egress    | configurable CIDR list (default `0.0.0.0/0` minus RFC1918) | `443`     | rclone egress from worker pods to S3 / GCS / Azure Blob. Tighten `networkPolicy.allow.nodeEgressObjectStore.cidrs` to your bucket VPC CIDR in production. |
| `allow-operator-to-apiserver`   | egress    | `0.0.0.0/0` (apiserver Service IP is not selectable by a NetworkPolicy peer) | `443`, `6443` | controller-runtime list/watch traffic for the `vmafx-operator`. |
| `allow-node-metrics-ingress`    | ingress   | any in-namespace pod (or a narrower `fromPodSelector`) | `9090` | Prometheus scraping of the vmafx-node metrics endpoint. Tighten `networkPolicy.allow.nodeMetrics.fromPodSelector` to `{app.kubernetes.io/name: prometheus}` in production. |
| `allow-dns-egress`              | egress    | `kube-system` / CoreDNS pods                    | `53/udp`, `53/tcp`  | Cluster DNS resolution — required for the other allow-rules to function. |

Override knobs live under `networkPolicy.allow.*` in `values.yaml`; each
rule has its own `enabled` switch so you can disable specific flows when
your topology already covers them.

A NetworkPolicy-aware CNI (Cilium, Calico, kube-router, Antrea, ...) is
required for the policies to take effect.  Verify with:

```bash
kubectl get networkpolicy -n vmafx-prod -l app.kubernetes.io/instance=vmafx
```

## PodDisruptionBudget {#pod-disruption-budget}

Disabled by default (`podDisruptionBudget.enabled=false`). Enable for HA
deployments with `replicaCount >= 2` to prevent Kubernetes from evicting all
pods simultaneously during node drains, cluster upgrades, or voluntary
disruptions.

```yaml
podDisruptionBudget:
  enabled: true
  minAvailable: 1   # or maxUnavailable: 1
```

When enabled, the chart creates a `policy/v1 PodDisruptionBudget` for each
active pool (controller, node, operator). `minAvailable: 1` is the recommended
setting for a 2-replica deployment; for larger deployments consider a
percentage (`minAvailable: "50%"`).

Requires Kubernetes >= 1.21 (for `policy/v1`). See ADR-1058.

## Related

- [GPU scheduling guide](gpu-scheduling.md)
- [Production Dockerfile](../../deploy/Dockerfile) — ADR-0698
- [Cloud-native redesign](../../docs/adr/0697-vmafx-cloud-native-redesign.md) — ADR-0697
- [Helm chart ADR](../../docs/adr/0699-vmafx-helm-chart-k8s.md) — ADR-0699
- [Security hardening ADR](../../docs/adr/1058-helm-chart-security-hardening.md) — ADR-1058

<!-- markdownlint-disable MD013 MD060 -->
# Kubernetes Deployment (Helm)

Use the Helm chart under `deploy/helm/vmafx/` to run VMAFX on Kubernetes. It
supports three workload types (Deployment, Job, StatefulSet) and all three GPU
device-plugin vendors (NVIDIA, AMD, Intel).

The page runs from installation (Prerequisites, Quick start, GPU vendor
matrix, Workload types) through configuration (Environment variables,
Persistence, Scaling, Monitoring, Ingress) and operations (Common operations,
Upgrading from 1.0.0-rc.1) to hardening (Pod security, NetworkPolicy,
PodDisruptionBudget).

The chart installs four custom resource definitions from
`deploy/helm/vmafx/crds/`: `VmafxJob`, `VmafxNode` and `VmafxModelTraining`,
which the operator reconciles ([operator.md](operator.md)), and `VmafxTenant`,
which holds per-tenant OIDC and RBAC settings that the controller's auth
gateway reads and enforces (`auth.tenants` in `values.yaml`; see
[server/auth.md](../server/auth.md#helm-configuration)). The auth gateway
belongs to `vmafx-controller`, not to the chart's default `vmafx-server`
image: with `auth.enabled`, set `image.repository` to a controller image, or
the render fails.

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
- A container runtime that pulls zstd layers: containerd 1.6 or later (what
  Kubernetes 1.26 requires anyway), CRI-O, or Docker Engine 23.0 or later behind
  cri-dockerd. The images published after `v1.0.0-rc.2` have zstd layers
  ([what can pull them](../usage/docker.md#what-can-pull-the-images)).
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
| `amd` | `amd.com/gpu` | `hip` | [AMD ROCm device plugin](https://github.com/ROCm/k8s-device-plugin) |
| `intel` | `gpu.intel.com/i915`, or `gpu.intel.com/xe` with `gpu.intelDriver: xe` | `sycl` | [Intel GPU plugin](https://github.com/intel/intel-device-plugins-for-kubernetes) |
| `cpu` | _(none)_ | `cpu` | _(none)_ |

`gpu.resourceName` requests any other extended resource verbatim (a GPU
sharing resource or an NVIDIA MIG slice); see the
[GPU scheduling guide](gpu-scheduling.md#how-gpu-device-plugins-work).

The chart automatically sets the `VMAFX_BACKEND` environment variable inside
the container based on `gpu.vendor`, so the VMAFX runtime picks the correct
backend without further configuration.

The Vulkan backend was removed in
[ADR-0726](../adr/0726-drop-vulkan-backend.md). Supported backends are `cuda`,
`hip`, `sycl`, and `cpu`; see the
[GPU scheduling guide](gpu-scheduling.md#backend-selection).

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
- `GET /metrics` — Prometheus metrics (optional; enable
  `monitoring.enabled=true`)

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

## Controller, nodes and operator {#controller}

`controller.enabled` deploys `vmafx-controller`, the job queue, node API and
auth gateway of the distributed platform
([ADR-1589](../adr/1589-helm-controller-workload.md)):

```yaml
controller:
  enabled: true
  persistence:
    size: 5Gi              # the SQLite job queue at /data
auth:
  enabled: true            # the controller needs auth settings
  issuer: https://idp.example.com/
  jwksEndpoint: https://idp.example.com/.well-known/jwks.json
  scoringRoots: ["/media/{tenant}"]   # inputs callers may score (ADR-1577)
node:
  enabled: true            # registers with the controller automatically
  controllerToken:
    secretName: vmafx-node-token      # key "token": a JWT with vmafx:node
operator:
  enabled: true            # polls the controller's GetJob
  controllerToken:
    secretName: vmafx-operator-token  # key "token": a JWT with vmafx:reader
```

What the chart renders:

- **`<release>-controller` Deployment.** One replica with the `Recreate`
  strategy: the queue is an embedded SQLite database, so two pods would each
  own a different queue. `VMAFX_DB_PATH=/data/vmafx-controller.db` on a
  `ReadWriteOnce` PersistentVolumeClaim (`controller.persistence`;
  `existingClaim` reuses one, `enabled: false` uses an emptyDir and loses the
  queue with the pod). Liveness and readiness probe `/healthz` and `/readyz`
  on the HTTP port. Image `ghcr.io/vmafx/vmafx-controller:v<appVersion>`.
  The single replica is transitional:
  [ADR-2350](../adr/2350-cloud-native-platform.md) moves the job state to
  PostgreSQL so the controller can run several replicas; this section changes
  when that store ships.
- **`<release>-controller` service account**, used only by the controller
  pods and the only account bound to the `VmafxTenant` reader Role
  ([ADR-1592](../adr/1592-helm-split-service-accounts.md)); the server, job
  and node pods share the chart's account, which holds no RBAC.
- **`<release>-controller` Service** with `http` (`controller.httpPort`,
  8080: `/healthz`, `/readyz`, `/metrics`, `POST /v1/score`) and `grpc`
  (`controller.grpcPort`, 9090: `VmafxController`, `VmafxScoring`).
- **Auth settings.** Everything under `auth.*` configures the controller only
  ([auth guide](../server/auth.md#helm-configuration)). `controller.enabled`
  and `auth.enabled` go together; for a cluster without an identity provider
  set `auth.disabled: true` (development only).
- **Nodes** get `VMAFX_CONTROLLER_ADDR=<release>-controller.<namespace>.svc:9090`
  unless `node.controllerAddr` names another controller.
- **Operator** gets `VMAFX_CONTROLLER_GRPC_ADDR` and `VMAFX_CONTROLLER_HTTP_ADDR`
  of the same Service.
- **Tokens.** `node.controllerToken` / `operator.controllerToken` mount key
  `key` (default `token`) of Secret `secretName` read-only at
  `/var/run/secrets/vmafx/controller-token/token` and set
  `VMAFX_CONTROLLER_TOKEN_FILE`. Both programs read the file on every call, so
  updating the Secret rotates the token without a restart (the kubelet
  refreshes the mounted file after its sync period). A node token carries
  `vmafx:node`, an operator token `vmafx:reader` of the tenant whose
  `VmafxJob`s it tracks.

The chart refuses `image.repository` naming a `vmafx-controller` image: the
server workload (`workload`, default `Deployment` with `vmafx-server`) no
longer receives auth settings, so a controller there would run without them.
Move such a release to `controller.enabled` (see
[upgrading](#upgrading-to-the-controller-workload)).

The controller image is published with the other Go images by
`docker-publish-operator-node.yml`, with licence notices in the image, signed
SBOMs and a `<tag>-source` image (see [what is signed](release.md#what-is-signed)).
Its GHCR package is new: the first release that publishes it creates it, and
until a maintainer has checked that it is public
([making the images public](release.md#making-the-container-images-public))
anonymous pulls can fail.

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

# Rolling update to a new image (replace the tag with a published release,
# for example v1.0.0-rc.2)
kubectl set image -n vmafx deployment/vmafx \
  vmafx=ghcr.io/vmafx/vmafx-server:<release tag>
```

The chart's `image.repository` defaults to `ghcr.io/vmafx/vmafx-server`, the
Go server image. The release workflow `docker-publish-operator-node.yml`
publishes it, together with the operator and node images, when a release is
published. The CLI and GPU images (`ghcr.io/vmafx/vmafx`) are documented in
[docker-production.md](docker-production.md).

The controller Deployment and the vmafx-node worker Deployment both use
`RollingUpdate` with `maxUnavailable: 0` and `maxSurge: 1` by default,
ensuring zero-downtime updates and preventing GPU pod eviction before
replacements are ready (ADR-1094). The grace period defaults to 60 s
(`terminationGracePeriodSeconds: 60`), giving in-flight scoring jobs time
to finish before SIGKILL. Raise this to 300 s or more for long CHUG
extractions:

```yaml
terminationGracePeriodSeconds: 300
```

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

## Upgrading from 1.0.0-rc.1 {#upgrading-from-100-rc1}

A release installed from the v1.0.0-rc.1 chart needs one manual step before
its first `helm upgrade` to a later chart. Since 1.0.0-rc.2 the server
Deployment (or StatefulSet) selects `app.kubernetes.io/component: server` as
well as the release labels. The rc.1 selector held only the release labels, so
it also matched the operator, node and `helm test` Pods
([ADR-1353](../adr/1353-helm-server-component-selector.md)). Kubernetes does
not allow a workload's selector to change, so a plain upgrade fails:

```text
Error: UPGRADE FAILED: ... Deployment.apps "vmafx" is invalid: spec.selector:
Invalid value: {"matchLabels":{"app.kubernetes.io/component":"server",...}}:
field is immutable
```

Delete the server workload and upgrade. `--cascade=orphan` keeps its Pods
running; the new Deployment or StatefulSet adopts them and then rolls them to
the new version as usual:

```bash
kubectl delete deployment,statefulset -n vmafx --cascade=orphan \
  -l app.kubernetes.io/instance=vmafx,app.kubernetes.io/component=server
helm upgrade vmafx deploy/helm/vmafx/ -n vmafx --reuse-values
```

Replace `vmafx` in `-n vmafx` and `app.kubernetes.io/instance=vmafx` with your
namespace and release name. The label selector removes only the server
workload: the operator and node Deployments carry their own component labels.
A StatefulSet keeps its `state-*` PersistentVolumeClaims, and the new
StatefulSet binds them again. Without `--cascade=orphan` the server Pods are
deleted with the workload, and scoring is unavailable until the upgrade has
started new ones.

Alternatively, uninstall and install again:

1. `helm uninstall vmafx -n vmafx`
2. Run the `helm upgrade --install` command from [Quick start](#quick-start).

This removes the operator and node workloads too, and leaves PVCs in place (see
[Uninstall](#uninstall)).

Argo CD and Flux report the same immutable-field error. Delete the server
workload the same way and let them sync again. Releases installed from
1.0.0-rc.2 or later upgrade without this step.

## Upgrading to the controller workload {#upgrading-to-the-controller-workload}

Before [ADR-1589](../adr/1589-helm-controller-workload.md) a controller ran
as the server workload with `image.repository` set to a self-built
`vmafx-controller` image, and `auth.*` was rendered into that Deployment.
That form now fails to render. Replace it:

```yaml
# before
image:
  repository: registry.example.com/vmafx-controller
auth:
  enabled: true
  # ...
# after
controller:
  enabled: true
  image:
    repository: registry.example.com/vmafx-controller   # or the published image
auth:
  enabled: true
  # ...
```

The job queue of the old Deployment lived wherever its `VMAFX_DB_PATH`
pointed (an emptyDir unless you mounted a volume); copy the database to the
new `<release>-controller-data` claim before the first start if you need its
jobs. Nodes and the operator follow the new Service by themselves.

## Pod security {#pod-security}

Every pod the chart emits — controller `Deployment`, batch `Job`, sticky
`StatefulSet`, `vmafx-node` worker `Deployment`, and the `vmafx-operator`
`Deployment` — satisfies the Kubernetes [Pod Security Admission
"restricted"](https://kubernetes.io/docs/concepts/security/pod-security-admission/)
profile (ADR-0930):

| Setting                          | Value                              | Why                                                                                          |
|----------------------------------|------------------------------------|----------------------------------------------------------------------------------------------|
| `runAsNonRoot`                   | `true`                             | Required by `restricted`; matches the `USER nonroot:nonroot` directive in every production image (ADR-0878). |
| `runAsUser` / `runAsGroup`       | `65532`                            | Distroless `gcr.io/distroless/cc-debian13` baked-in nonroot UID/GID — keeps file ownership consistent across `emptyDir`, PVCs, and rclone caches. |
| `readOnlyRootFilesystem`         | `true`                             | Writes are restricted to explicitly-mounted `emptyDir` / PVC volumes (`/tmp`, the StatefulSet's `/var/lib/vmafx`).  Catches privilege-escalation primitives that depend on overwriting on-disk binaries. |
| `allowPrivilegeEscalation`       | `false`                            | Drops the `no_new_privs` exec bit; covers the SUID and `cap_setuid` escape paths.            |
| `capabilities.drop`              | `[ALL]`                            | Distroless containers do not need `CAP_NET_BIND_SERVICE` etc.; everything is dropped.        |
| `seccompProfile.type`            | `RuntimeDefault`                   | Engages the container-runtime default syscall filter (Docker/containerd ship a reasonable allow-list).  Required by `restricted` since k8s 1.25. |

To enforce the profile cluster-side, label your install namespace
([k8s
docs](https://kubernetes.io/docs/concepts/security/pod-security-admission/#pod-security-admission-labels-for-namespaces)):

```bash
kubectl label --overwrite namespace vmafx-prod \
  pod-security.kubernetes.io/enforce=restricted \
  pod-security.kubernetes.io/audit=restricted \
  pod-security.kubernetes.io/warn=restricted
```

If your image requires write access outside the mounted volumes, override
`podSecurityContext` / `securityContext` in `values.yaml` — but doing so
moves the namespace out of the `restricted` profile.

Two node settings leave the profile on purpose
([ADR-1593](../adr/1593-helm-node-fuse-and-ebpf.md)), and with either of them
the node pods need a namespace that allows `privileged`:

| Setting | Node container | Why |
| --- | --- | --- |
| `node.fuse` (needed by `storage.mode: mount`) | UID 65532, `allowPrivilegeEscalation: true`, capabilities `[SYS_ADMIN, DAC_READ_SEARCH]`, one `/dev/fuse` from a device plugin's resource | the setuid `fusermount3` mounts with these; the node process has no effective capability ([node guide](../server/node.md#kubernetes-deployment)) |
| `node.ebpf` (with `node.fuse`) | UID 0, capabilities `[BPF, PERFMON, SYS_ADMIN]`, the host's `/sys/kernel/tracing` read-only | the eBPF tracker loads its program as root ([eBPF tracker](ebpf-fuse-bypass.md#kubernetes)) |

## NetworkPolicy {#networkpolicy}

Disabled by default (`networkPolicy.enabled=false`) because many clusters
either ship their own CNI-managed policies (Cilium ClusterwideNetworkPolicy,
Calico GlobalNetworkPolicy) or do not install a NetworkPolicy controller —
in the latter case the chart's NetworkPolicies render but are inert.

Opt in with `--set networkPolicy.enabled=true`.  The chart then emits a
default-deny baseline plus narrow allow-rules (the controller rules only with
`controller.enabled`, `allow-node-to-controller` when the nodes have a
controller, `allow-controller-to-apiserver` only with a tenant registry):

| Policy                          | Direction | Peer                                            | Ports               | Purpose                                              |
|---------------------------------|-----------|-------------------------------------------------|---------------------|------------------------------------------------------|
| `default-deny`                  | both      | _(no allow)_                                    | _(all)_             | Safety net — drops everything that is not explicitly allowed. Emitted per workload component (root / operator / node) so a new component without an allow-rule remains isolated. |
| `allow-http-ingress`            | ingress   | every pod in the release namespace              | `service.targetPort`| Scoring server reachable from any in-namespace client. |
| `allow-controller-to-node`      | ingress   | controller pods (selector match)                | `node.grpcPort` (50052; `nodePort` overrides) | gRPC dispatch from controller to `vmafx-node` workers. |
| `allow-controller-ingress`      | ingress   | every pod in the release namespace (`controllerIngress.fromPodSelector` narrows) | `controller.httpPort`, `controller.grpcPort` | Nodes, operator and in-namespace clients reach the controller. |
| `allow-operator-to-controller`  | egress    | the chart's controller pods                     | `controller.grpcPort`, `controller.httpPort` | The operator's `GetJob` polls and `/healthz` probes. |
| `allow-controller-to-identity-provider` | egress | `controllerToIdentityProvider.cidrs` (default `0.0.0.0/0`) | `443` | JWKS fetches while auth is on (not with `auth.disabled`). |
| `allow-node-to-controller`      | egress    | the chart's controller pods with `controller.enabled`, else pods matching `networkPolicy.allow.nodeToController.podSelector` (default: every pod in the namespace) | `controller.grpcPort`, else `nodeToController.port` (9090) | The nodes' controller client (RegisterNode, Heartbeat, PullWork, ReportResult). Rendered when the nodes have a controller. |
| `allow-node-egress-object-store`| egress    | configurable CIDR list (default `0.0.0.0/0` minus RFC1918) | `443`     | rclone egress from worker pods to S3 / GCS / Azure Blob. Tighten `networkPolicy.allow.nodeEgressObjectStore.cidrs` to your bucket VPC CIDR in production. |
| `allow-operator-to-apiserver`   | egress    | `0.0.0.0/0` (apiserver Service IP is not selectable by a NetworkPolicy peer) | `443`, `6443` | controller-runtime list/watch traffic for the `vmafx-operator`. |
| `allow-controller-to-apiserver` | egress    | `0.0.0.0/0` (same reason)                       | `443`, `6443`       | The controller listing `VmafxTenant` resources; rendered with `auth.enabled` and `auth.tenants` / `auth.tenantSource: kubernetes` (values key `serverToApiserver`). |
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
deployments to prevent Kubernetes from evicting all pods simultaneously during
node drains, cluster upgrades, or voluntary disruptions.

```yaml
podDisruptionBudget:
  enabled: true
  # maxUnavailable: 1  — default: allows one voluntary disruption at a time.
  # Use this for all replica counts, including single-replica dev deployments.
  maxUnavailable: 1
```

The default strategy is `maxUnavailable: 1`. Do **not** use `minAvailable: 1`
with a single-replica Deployment — Kubernetes cannot satisfy `minAvailable: 1`
while draining the only pod, permanently blocking node drain operations. Switch
to `minAvailable` only when `replicaCount >= 2` and you need a hard lower-bound
on serving capacity:

```yaml
podDisruptionBudget:
  enabled: true
  minAvailable: 2   # requires replicaCount >= 3
```

When enabled, the chart creates a `policy/v1 PodDisruptionBudget` for each
active pool (controller, node, operator).

Requires Kubernetes >= 1.21 (for `policy/v1`). See ADR-1058, ADR-1094.

## Related

- [GPU scheduling guide](gpu-scheduling.md)
- [Production Dockerfile](../../docker/Dockerfile.production) — ADR-0698
- [Cloud-native server foundation](../adr/0701-vmafx-cloud-native-redesign.md) —
  ADR-0701
- [Helm chart ADR](../../docs/adr/0699-vmafx-helm-chart-k8s.md) — ADR-0699
- [Security hardening ADR](../../docs/adr/1058-helm-chart-security-hardening.md)
  — ADR-1058
- [Rolling-update correctness
  ADR](../../docs/adr/1094-helm-rolling-update-correctness.md) — ADR-1094

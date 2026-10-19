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
for the rationale. The values the chart copies into pod specs (tolerations,
affinity, security contexts, probes, volumes, update strategies, node
selectors, labels and annotations) have the types Kubernetes 1.26, the oldest
supported release, gives them
([what that refuses](#upgrading-to-the-kubernetes-typed-values-schema)). Both
files are generated from `api/vmafx-platform.toml`
([API generation](api-generation.md#helm-chart-values)).

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
- For the controller on PostgreSQL with the chart's database
  (`controller.store.postgresql.mode: cnpg`): the CloudNativePG operator — see
  [job store and replicas](#controller-store)

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

The chart sets the node's `VMAFX_BACKEND` environment variable from
`gpu.vendor`, so a node runs and advertises the matching backend without
further configuration. The scoring server (`workload`) takes its backend per
request, from the `backend` score option
([score options](../server/api-contract.md)); the chart sets no backend
variable on it.

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

- **`<release>-controller` Deployment.** Where it keeps its jobs decides its
  shape ([job store and replicas](#controller-store)): with the default
  SQLite store one replica, the `Recreate` strategy and the queue on a
  claim; with the PostgreSQL store `controller.replicas` replicas and a
  rolling update. Liveness and readiness probe `/healthz` and `/readyz` on
  the HTTP port. Image `ghcr.io/vmafx/vmafx-controller:v<appVersion>`.
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
- **Operator RBAC** (`operator.enabled`). The operator's service account is
  bound to three roles. `<release>-operator-crds` is a `ClusterRole` for
  `VmafxJob`, `VmafxNode` and `VmafxModelTraining` in every namespace, with
  their status and finalizers. `<release>-operator-events` is a `ClusterRole`
  that may only create and patch events, in every namespace: the operator
  records events on the resources it reconciles, and Kubernetes stores an
  event in the namespace of its resource
  ([ADR-2647](../adr/2647-operator-events-cluster-wide.md)).
  `<release>-operator-ns` is a `Role` in the release namespace for pods and
  the leader-election lease ([ADR-1058](../adr/1058-helm-chart-security-hardening.md)).
  None of them grants access to `VmafxTenant`.
- **Tokens.** `node.controllerToken` / `operator.controllerToken` mount key
  `key` (default `token`) of Secret `secretName` read-only at
  `/var/run/secrets/vmafx/controller-token/token` and set
  `VMAFX_CONTROLLER_TOKEN_FILE`. Both programs read the file on every call, so
  updating the Secret rotates the token without a restart (the kubelet
  refreshes the mounted file after its sync period). A node token carries
  `vmafx:node`, an operator token `vmafx:reader` of the tenant whose
  `VmafxJob`s it tracks.

### Job store and replicas {#controller-store}

`controller.store.backend` chooses where the controller keeps jobs, attempts
and node sessions ([ADR-2350](../adr/2350-cloud-native-platform.md); the
controller side is in [job persistence](../server/controller.md#job-persistence)):

| `controller.store.backend` | Replicas | Update strategy | State |
| --- | --- | --- | --- |
| `sqlite` (default) | 1 (the chart refuses more) | `Recreate` | `VMAFX_DB_PATH=/data/vmafx-controller.db` on a `ReadWriteOnce` claim (`controller.persistence`; `existingClaim` reuses one, `enabled: false` uses an emptyDir and loses the queue with the pod) |
| `postgres` | `controller.replicas` (1 or more) | `RollingUpdate`, `maxUnavailable: 0`, `maxSurge: 1` | PostgreSQL; no claim |

Two controllers on SQLite would each own a different queue, so the SQLite
store stays one replica. With PostgreSQL every replica serves every request:
nodes keep their sessions and leases when a replica goes away, and a job
whose node dies returns to the queue when its lease expires.

```yaml
controller:
  enabled: true
  replicas: 3
  store:
    backend: postgres
    postgresql:
      mode: cnpg            # or external
```

**CloudNativePG (`postgresql.mode: cnpg`, the default).** The chart renders a
`postgresql.cnpg.io/v1` `Cluster` named `<release>-db` and refuses to render
when the API is missing: the [CloudNativePG](https://cloudnative-pg.io/)
operator is a prerequisite you install once per cluster, as the chart does
not install operators. The kind test installs its release manifest, 1.30.1:

```bash
kubectl apply --server-side -f \
  https://github.com/cloudnative-pg/cloudnative-pg/releases/download/v1.30.1/cnpg-1.30.1.yaml
```

| Value | Default | Meaning |
| --- | --- | --- |
| `controller.store.postgresql.cnpg.instances` | `1` | PostgreSQL instances (a primary and `instances - 1` replicas) |
| `controller.store.postgresql.cnpg.imageName` | PostgreSQL 18.6, pinned by digest | Operand image |
| `controller.store.postgresql.cnpg.storage.size` / `.storageClass` | `5Gi` / cluster default | Volume of each instance |
| `controller.store.postgresql.cnpg.resources` | `{}` | Requests and limits of the database pods |

The operator creates database `vmafx` owned by role `vmafx` and a Secret
`<release>-db-app`; the controller reads `VMAFX_DB_DSN` from its `uri` key.
Role `vmafx` is not a superuser, so row-level security applies to it.

**External database (`postgresql.mode: external`).** Name a Secret that holds
a connection URI, for example
`postgres://vmafx:…@db.example:5432/vmafx?sslmode=verify-full`, in
`controller.store.postgresql.external.secretName` (key `secretKey`, default
`uri`). The role must not be a superuser.

**Schema migration.** A Job `<release>-controller-migrate-<hash>` runs
`vmafx-controller migrate` with the controller image
(`controller.store.migration.backoffLimit` retries, default 10). The hash
comes from the Job's pod template, so an upgrade that changes the image (or
another setting of the Job) runs a new Job once and Helm removes the previous
one, while an upgrade that changes neither keeps the completed Job. Controllers report ready
only when the database holds the schema they need, so a rolling update waits
for the migration. The Job carries Argo CD `Sync` hook annotations for
GitOps installs. To move jobs from an earlier SQLite queue, see
[moving from the SQLite queue](../server/controller.md#moving-from-the-sqlite-queue).

**Lease and session lifetimes.** `controller.store.leaseTTL`, `sessionTTL`,
`sweepInterval`, `backoffBase` and `backoffMax` (durations such as `30s` or
`2m`) set `VMAFX_STORE_LEASE_TTL`, `VMAFX_STORE_SESSION_TTL`,
`VMAFX_STORE_SWEEP_INTERVAL`, `VMAFX_STORE_BACKOFF_BASE` and
`VMAFX_STORE_BACKOFF_MAX`. Left empty, the controller's defaults apply
(60 s, 60 s, 5 s, 5 s, 5 min).

**Several replicas.** With `controller.replicas` above 1 the chart adds a
PodDisruptionBudget `<release>-controller` (`maxUnavailable: 1`) and, unless
`controller.topologySpreadConstraints` lists your own, spreads the replicas
across nodes (`kubernetes.io/hostname`, `ScheduleAnyway`). With
`networkPolicy.enabled` the rule `allow-controller-to-database` lets the
controller and migration pods reach the database (see
[NetworkPolicy](#networkpolicy)).

**Tested on kind.** The E2E suite's `02-controller-ha` case
(`test/e2e/kuttl-tests/02-controller-ha/`) installs this configuration with
two replicas, kills one replica and the node of a running job mid-job, and
checks in the database that every job was completed by exactly one attempt.

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

Every variable a binary reads, and the chart value that sets it, is in the
binary's generated table: [controller](../server/controller.md#configuration),
[server](../server/grpc.md#configuration),
[node](../server/node.md#configuration-12-factor-env-vars) and
[operator](../server/operator.md#configuration-12-factor-env-vars). The chart
writes those entries from `templates/_config.gen.tpl`, generated from
`api/vmafx-platform.toml`
([API generation](api-generation.md#environment-of-the-go-binaries)).

The server also reads the keys of the `config:` map, which the chart renders
into a ConfigMap (`config.VMAFX_MODEL_DIR`, for example), and every workload
takes the `env:` map as extra variables:

```yaml
# values.yaml override
env:
  VMAFX_LOG_LEVEL: debug
  VMAFX_HTTP_TIMEOUTS_WRITE: "30m"
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

The vmafx-node worker Deployment, and the controller Deployment on the
PostgreSQL store, use `RollingUpdate` with `maxUnavailable: 0` and
`maxSurge: 1`, ensuring zero-downtime updates and preventing GPU pod
eviction before replacements are ready (ADR-1094). The controller on the
SQLite store uses `Recreate` ([job store and replicas](#controller-store)). The grace period defaults to 60 s
(`terminationGracePeriodSeconds: 60`), giving in-flight scoring jobs time
to finish before SIGKILL. Raise this to 300 s or more for long CHUG
extractions:

```yaml
terminationGracePeriodSeconds: 300
```

## Monitoring

`monitoring.enabled` renders a ServiceMonitor for the server, the controller
and the nodes, a PodMonitor for the operator, a PrometheusRule with the alerts
and recording rules, and a ConfigMap per Grafana dashboard (requires the
[prometheus-operator](https://github.com/prometheus-operator/prometheus-operator)
CRDs):

```yaml
monitoring:
  enabled: true
  serviceMonitor:
    labels:
      release: prometheus    # match your Prometheus operator selector
    interval: 30s
```

[Monitoring on Kubernetes](../observability/kubernetes.md) covers the
component switches, the SLO objectives and alert thresholds, the dashboard
sidecar labels and the scrape NetworkPolicy.

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

## Upgrading to the Kubernetes-typed values schema {#upgrading-to-the-kubernetes-typed-values-schema}

From this release the values schema checks the values the chart copies into
pod specs against the Kubernetes 1.26 types of those fields
([ADR-2350](../adr/2350-cloud-native-platform.md) D13). Before, each of these
keys accepted any list or any mapping, and a wrong value surfaced only when
the API server refused the rendered object or, for a field it does not check,
not at all. Now `helm install`, `helm upgrade` and `helm lint` refuse it and
name the key:

| Key | Now refused |
| --- | --- |
| `tolerations`, `controller.tolerations`, `node.tolerations` | A list item that is not a toleration, or a toleration field of another type, such as `tolerationSeconds: "60"` (a number is required) |
| `affinity` | A value that is not an `Affinity`: a list where a term mapping belongs, a node selector term without `nodeSelectorTerms`, a pod affinity term without `topologyKey` |
| `topologySpreadConstraints`, `controller.topologySpreadConstraints` | A constraint without `maxSkew`, `topologyKey` or `whenUnsatisfiable`, or with a field of another type |
| `podSecurityContext` | A field of another type, such as `runAsUser: root` (a user ID is a number) |
| `securityContext` | A field of another type, such as `capabilities: {drop: ALL}` (`drop` is a list) |
| `livenessProbe`, `readinessProbe` | A field of another type, such as `periodSeconds: often` |
| `node.volumes`, `node.volumeMounts` | A volume without `name`, a mount without `name` or `mountPath`, a field of another type |
| `deployment.strategy`, `node.strategy`, `statefulSet.updateStrategy` | A field of another type, such as a list for `maxSurge` |
| `envFrom` | A list item that is not an `EnvFromSource` (`configMapRef`, `secretRef`, `prefix`) |
| `ingress.tls` | An item whose `hosts` is not a list of names, or whose `secretName` is not a string |
| `nodeSelector`, `controller.nodeSelector`, `node.nodeSelector`, `podAnnotations`, `serviceAccount.annotations`, `ingress.annotations`, `monitoring.serviceMonitor.labels` | A value that is not a string, such as `gpu: 1` (write `gpu: "1"`) |

Fields Kubernetes does not know are still accepted inside these types, as the
API server accepts them. The API server refuses each refused value too once
the chart puts it into an object; the schema checks it earlier, and also when
the workload that would use it is disabled (`node.volumes` with
`node.enabled: false`, for example), where it used to be ignored. Write the
value as the Kubernetes type has it; the error message names the key and the
type it expected, for example `at '/tolerations/0/tolerationSeconds': got
string, want integer`.

The `resources` keys accept more than before: a quantity may be a decimal
number (`cpu: 1.5`), and `claims` is accepted as Kubernetes 1.26 defines it.
`imagePullSecrets` keeps the chart's stricter rule that every entry names a
Secret.

## Upgrading to the generated workload environment {#upgrading-to-the-generated-workload-environment}

The chart writes every `VMAFX_*` variable of its workloads from the platform
definition ([ADR-2350](../adr/2350-cloud-native-platform.md) D13). The
scoring server's Deployment, StatefulSet and Job no longer get
`VMAFX_BACKEND`: `vmafx-server` never read it and takes its backend from each
request's `backend` score option. The install notes no longer print a
`BACKEND` line. The server container has an `env` list only when `env` holds
values. Nodes keep `VMAFX_BACKEND`, set from `gpu.vendor` as before. A script
that read the variable from the server pod reads it from a node pod instead.

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
| `allow-controller-to-database` | egress | the chart's CloudNativePG `Cluster` pods (`cnpg.io/cluster`), or `controllerToDatabase.cidrs` (default `0.0.0.0/0`) with an external database | `controllerToDatabase.ports` (`5432`) | The controller and its migration Job reach PostgreSQL; rendered with `controller.store.backend: postgres`. |
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
active pool (server, node, operator). The controller has its own,
`maxUnavailable: 1`, whenever `controller.replicas` is above 1, whether or not
`podDisruptionBudget.enabled` is set ([job store and replicas](#controller-store)).

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

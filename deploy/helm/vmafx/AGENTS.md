<!-- markdownlint-disable MD013 -->
# AGENTS.md — deploy/helm/vmafx/

VMAFX Helm chart notes.

## Invariants

### podSecurityContext must always include seccompProfile (ADR-0969)

Workload templates (`deployment.yaml`, `job.yaml`, `statefulset.yaml`,
`node-deployment.yaml`, `operator-deployment.yaml`) render pod
`.Values.podSecurityContext`. K8s PSA restricted profile requires
`seccompProfile.type`: `RuntimeDefault` or `Localhost`. Default in `values.yaml`:

```yaml
podSecurityContext:
  ...
  seccompProfile:
    type: RuntimeDefault
```

**New templates rendering pod spec inherit `podSecurityContext`
verbatim**; never omit `seccompProfile`. ADR-0969, ADR-0930.

### Node-worker image must use the vmafx.nodeImage helper (ADR-0969)

`templates/node.yaml` renders node image:

```yaml
image: {{ include "vmafx.nodeImage" . }}
```

Values name `ghcr.io/vmafx/vmafx-node`. Helper retains `<image.repository>-node`
fallback if `node.image.repository` empty; tag defaults `v<Chart.AppVersion>`.
Never replace helper with inline expression.

### ADR-0930 follow-up

PR #439 (ADR-0930) changes `runAsUser`/`runAsGroup` from `65534` to
`65532` (distroless nonroot, ADR-0878); adds container-scope `seccompProfile`.
Update note on merge: final UID + container-scope seccompProfile set.

## Invariants (ADR-1047)

- `storage` stays in `values.yaml`, `mode: "http-serve"` default; schema
  defines key non-required; `additionalProperties: false` enforces user
  `storage.*` keys match schema. `mode` enum = node's `storage.ParseMode`
  set (`http-serve | mount | auto`, ADR-1526); never re-add `rclone`.
  `VMAFX_RCLONE_CONFIG` rendered only with `storage.rclone.config`.
- `gpu.count` minimum = 1; never lower to 0 (0 GPUs + device plugin = silent no-op).
- `auth` + `otelCollector` use `additionalProperties: true` (user-extensible
  oidc/rbac, otel exporter blocks).
- `networkPolicy.allow` uses `additionalProperties: false` (ADR-1058); rules
  must enumerate in `values.schema.json` under `allow` object properties.
  Never revert `additionalProperties: true` without ADR.

## Invariants (ADR-1058)

- **RBAC split**: ClusterRole (`*-operator-crds`) covers CRD resources only.
  Namespaced resources (pods, events, leases) sit in namespace Role
  (`*-operator-ns`). Never merge into single ClusterRole.
- **VmafxTenant only for the controller (ADR-1592, replaces ADR-1058's
  rule)**: no `vmafxtenants` rule in the operator ClusterRole (no tenant
  reconciler exists). Tenant-reader RoleBinding binds only
  `vmafx.controllerServiceAccountName`, used only by controller pods. Guard:
  `scripts/ci/tests/test_helm_service_accounts.py`.
- **PDB template**: `templates/pdb.yaml` uses `policy/v1` (k8s >= 1.21).
  Older clusters require `capabilities.apiVersions.has` guard.
- **Metrics NetworkPolicy**: `networkPolicy.allow.nodeMetrics` in schema.
  Default `fromPodSelector: {}` allows in-namespace pod scraping;
  production narrows to Prometheus selector.

## Invariants (ADR-1094)

- **node Deployment strategy**: `templates/node.yaml` requires explicit
  `spec.strategy` (`node.strategy`). K8s defaults 25%/25% evict GPU pods
  before replacement, dropping scoring jobs. Never drop `strategy:` block.
- **node probes use tcpSocket, not httpGet**: vmafx-node exposes gRPC server
  only; no HTTP listener. Probes use `tcpSocket` on `port: grpc`
  (`node.grpcPort`, default 50052). If PR adds HTTP metrics/health endpoint,
  upgrade to `httpGet` after verifying endpoint in image.
- **terminationGracePeriodSeconds must be set on all pod specs**: templates
  (`deployment.yaml`, `statefulset.yaml`, `node.yaml`) set `terminationGracePeriodSeconds`
  from `.Values.terminationGracePeriodSeconds`. New templates must match;
  K8s 30 s default insufficient for GPU scoring jobs.
- **PDB default is maxUnavailable, not minAvailable**: chart default =
  `maxUnavailable: 1`. `minAvailable` opt-in (`replicaCount >= 2`).
  Setting `minAvailable: 1` as default blocks node drain on single replica.
- **node Service name is `vmafx-node` (gRPC port)**: renamed from
  `vmafx-node-metrics` (phantom port 9090) to `vmafx-node` (gRPC port 50052)
  in ADR-1094. Update external tooling (NetworkPolicy selectors, ServiceMonitors).

## Operator env-only contract (ADR-1119 / ADR-1129)

`cmd/vmafx-operator` accepts `--version` only; runtime env-only.
`templates/operator-deployment.yaml` uses compound env vars
`VMAFX_OPERATOR_METRICS_ADDR=:8080`,
`VMAFX_OPERATOR_HEALTH_PROBE_ADDR=:8081`,
`VMAFX_OPERATOR_LEADER_ELECTION`, plus `VMAFX_LOG_LEVEL`. Never restore removed
pre-fx CLI flags. Ports and probes align metrics `8080` and health/readiness `8081`.

## Server component selector

HTTP Service selects `app.kubernetes.io/component: server` plus
release name/instance. Server workload Pod templates (Deployment,
StatefulSet, Job) carry label; operator/node use own labels.
Without discriminator, operator adds metrics port 8080 behind scoring Service,
producing HTTP 404 responses. Align headless Service, main PDB, HTTP
NetworkPolicy, ServiceMonitor selectors with server label.

Deployment + StatefulSet `spec.selector` carry `component: server`
(ADR-1353, 1.0.0-rc.2). rc.1 selector (release labels only) matched operator,
node, `helm test` Pods. Workload selectors immutable: `spec.selector` changes
need ADR + upgrade path per `docs/development/k8s-deployment.md#upgrading-from-100-rc1`.
`scripts/ci/check-helm-selector-isolation.py` (`helm-chart.yml`) fails on
cross-component Pod match. New component: own `component` label in selector and
Pod template.

## Auth settings and tenant registry (ADR-1519)

- `templates/auth-validate.yaml` refuses `auth.enabled` with image named
  `*vmafx-server` (no auth gateway) or non-Deployment workload, tenant
  settings without `auth.enabled`, `auth.disabled` with tenant registry.
  Never relax: each case otherwise deploys server without requested auth.
- Tenant mode (`vmafx.tenantSource` = `kubernetes`, implied by non-empty
  `auth.tenants`): `deployment.yaml` passes `VMAFX_AUTH_TENANTS_SOURCE` +
  `_NAMESPACE` and NO global `VMAFX_JWKS_ENDPOINT` / `_AUTH_ISSUER` /
  `_AUDIENCE` / `_TENANT_CLAIM` / `_ROLES_CLAIM` (controller refuses globals
  next to tenant source). Globals only default `auth.tenants` entries in
  `tenant-crd-config.yaml`.
- `controller-tenant-rbac.yaml` (Role + RoleBinding, namespace only) and
  netpol `allow-server-to-apiserver` render in tenant mode; dropping either
  stops controller start (list of VmafxTenant fails).
- `enabled:` in `tenant-crd-config.yaml` uses `hasKey`, never
  `default true` (`false | default true` renders `true`; suspension vanished).
- Guard: `scripts/ci/tests/test_helm_controller_auth.py` (`helm-chart.yml`).
- Scoring roots (ADR-1577): `auth.scoringRoots` -> `VMAFX_SCORING_ROOTS`
  (joined with `,`) in single-provider mode only; `auth.tenants[].scoring.roots`
  -> VmafxTenant `spec.scoring.roots`. `auth-validate.yaml` refuses
  scoringRoots with registry or without `auth.enabled`. No default roots:
  controller denies every input until set.

## Controller workload (ADR-1589)

- `templates/controller.yaml`: replicas 1, strategy `Recreate`, `/data` =
  SQLite queue volume (`controller.persistence`) — transitional. ADR-2350
  WP17 store moves state to Postgres (CloudNativePG `Cluster` or external
  DSN), then: 2 replicas, RollingUpdate, PDB, topology spread, no claim.
  Until then do not raise replicas (two pods = two queues). Selector +
  Service carry `component: controller` (selector isolation check).
- Auth env only via `vmafx.controllerAuthEnv`; server `deployment.yaml` gets
  none. `auth-validate.yaml`: `auth.enabled` <-> `controller.enabled`;
  `*vmafx-controller` `image.repository` refused; `controller.env` auth keys
  refused.
- Nodes: `vmafx.nodeControllerAddr` (node.controllerAddr, else chart Service).
  Operator: controller addresses only with `controller.enabled`. Tokens:
  `vmafx.controllerTokenVolume` / `Mount` -> `VMAFX_CONTROLLER_TOKEN_FILE`.
- Guard: `scripts/ci/tests/test_helm_controller_workload.py` (`helm-chart.yml`).

## Node FUSE and eBPF (ADR-1593)

- Node container securityContext only via `vmafx.nodeSecurityContext`; never
  plain `toYaml .Values.securityContext` in `node.yaml`. `node.fuse`: UID
  65532 kept, bounding set `[SYS_ADMIN, DAC_READ_SEARCH]`,
  `allowPrivilegeEscalation: true` (setuid `fusermount3` only; measured:
  either missing -> mount fails). `node.ebpf`: UID 0,
  `[BPF, PERFMON, SYS_ADMIN]`, hostPath `/sys/kernel/tracing` read-only; no
  `/sys/kernel/btf` mount (container sysfs shows it).
- `/dev/fuse` only via `node.fuse.resourceName` (device-plugin resource);
  never a privileged fallback.
- `templates/node-validate.yaml` refuses: mount without fuse, fuse without
  resourceName, ebpf without mount + fuse, relative or >255-byte prefix, mount
  root outside prefix, `env.VMAFX_EBPF_*`, storage env keys with fuse.
- Mount root = `vmafx.nodeMountRoot`; emptyDir only outside `/tmp`
  (`vmafx.nodeMountRootVolume`).
- Guard: `scripts/ci/tests/test_helm_node_fuse_ebpf.py` (`helm-chart.yml`).

## Active GPU backends

Chart maps NVIDIA, AMD, Intel device-plugin resources to CUDA, HIP, SYCL.
Vulkan removed by ADR-0726; never describe as backend, fallback, or GPU capability.
Preserve removal notice in `templates/NOTES.txt`, `_helpers.tpl`, and K8s GPU
scheduling docs on rebase.

## References

- [ADR-0699](../../../docs/adr/0699-vmafx-helm-chart-k8s.md) — chart ADR
- [ADR-0930](../../../docs/adr/0930-helm-networkpolicy-pss.md) — PSS + NetworkPolicy
- [ADR-0969](../../../docs/adr/0969-helm-seccomp-default-plus-node-image-helper.md) — seccompProfile default + node image helper fix
- [ADR-1047](../../../docs/adr/1047-helm-schema-bug-fixes.md) — R9 schema correctness fixes
- [ADR-1058](../../../docs/adr/1058-helm-chart-security-hardening.md) — PDB, RBAC split, metrics NetworkPolicy, schema tightening
- [ADR-1074](../../../docs/adr/1074-helm-values-completeness.md) — nameOverride/fullnameOverride, statePVCSize, node.metricsPort, extraPorts items schema
- [ADR-1094](../../../docs/adr/1094-helm-rolling-update-correctness.md) — rolling-update strategy, probe fix, PDB default, grace period
- [ADR-1119](../../../docs/adr/1119-golusoris-go-framework-adoption.md) — env-only fx migration
- [ADR-1129](../../../docs/adr/1129-release-container-runtime-alignment.md) — release image/runtime alignment
- [ADR-0726](../../../docs/adr/0726-drop-vulkan-backend.md) — Vulkan backend removal
- [ADR-1353](../../../docs/adr/1353-helm-server-component-selector.md) — server workload component selector, rc.1 upgrade path
- [ADR-1519](../../../docs/adr/1519-controller-tenant-registry.md) — controller tenant registry, chart auth guards

## Invariants (ADR-1524)

- `node.controllerAddr` has NO default: `VMAFX_CONTROLLER_ADDR` rendered only
  when set (chart deploys no controller; old default pointed at a missing
  Service on the controller HTTP port). Empty = standalone node.
- `allow-node-to-controller` egress policy rendered only with
  `node.controllerAddr`; port = controller gRPC (9090). Schema entry
  `networkPolicy.allow.nodeToController` keeps `additionalProperties: false`.
- Guard: `scripts/ci/tests/test_helm_node_contract.py` (helm-chart workflow).

## Invariants (ADR-1547)

- GPU resource name resolved ONLY in `vmafx.gpuResourceName` (`_helpers.tpl`):
  `gpu.resourceName` verbatim > vendor default; intel ->
  `gpu.intel.com/<gpu.intelDriver>` (`i915` default, `xe`). Never hard-code
  `gpu.intel.com/i915` in a template or bring back `vmafx.gpuResourceKey`.
- `allow-controller-to-node` port = `controllerToNode.nodePort` or
  `node.grpcPort`; no fixed 50051.
- Guard: `scripts/ci/tests/test_helm_node_contract.py` (`GpuResource`,
  `ControllerToNodePort`).

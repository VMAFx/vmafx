<!-- markdownlint-disable MD013 -->
# AGENTS.md — deploy/helm/vmafx/

Contains VMAFX Helm chart. Notes for agents working here.

## Invariants

### podSecurityContext must always include seccompProfile (ADR-0969)

Every workload template (`deployment.yaml`, `job.yaml`, `statefulset.yaml`,
`node-deployment.yaml`, `operator-deployment.yaml`) renders pod-level
security context from `.Values.podSecurityContext`. Kubernetes PSA
"restricted" admission profile requires `seccompProfile.type` set to
`RuntimeDefault` or `Localhost`. Default value set in `values.yaml`:

```yaml
podSecurityContext:
  ...
  seccompProfile:
    type: RuntimeDefault
```

**Any new template rendering pod spec must inherit `podSecurityContext`
verbatim** — never hand-roll partial security context omitting
`seccompProfile`. See ADR-0969 and ADR-0930.

### Node-worker image must use the vmafx.nodeImage helper (ADR-0969)

`templates/node.yaml` renders node container image via:

```yaml
image: {{ include "vmafx.nodeImage" . }}
```

Shipped values name `ghcr.io/vmafx/vmafx-node` explicitly, matching
release publisher. Helper retains `<image.repository>-node` only as
fallback for custom values deliberately leaving `node.image.repository`
empty, defaults tag to canonical `v<Chart.AppVersion>` release
tag. Never replace helper with inline expression.

### ADR-0930 follow-up

PR #439 (ADR-0930) changes `runAsUser`/`runAsGroup` from `65534` to
`65532` (distroless nonroot per ADR-0878), adds container-scope
`seccompProfile`. When that PR merges, update this note: final UID +
container-scope seccompProfile also set.

## Invariants (ADR-1047)

- `storage` must remain in `values.yaml` with `mode: "http-serve"` as default;
  schema defines key as non-required, but `additionalProperties: false` means
  any user-supplied `storage.*` key must match schema definition.
- `gpu.count` minimum = 1; never lower back to 0 — 0 GPUs with vendor device
  plugin = silent no-op.
- `auth` + `otelCollector` use `additionalProperties: true` in schema
  intentionally — sub-keys are user-extensible (per-tenant oidc/rbac configs,
  arbitrary otel exporter blocks).
- `networkPolicy.allow` uses `additionalProperties: false` (changed in ADR-1058) —
  any new allow-rule must be enumerated in `values.schema.json` under `allow`
  object properties. Never revert to `additionalProperties: true` without
  corresponding ADR.

## Invariants (ADR-1058)

- **RBAC split**: operator ClusterRole (`*-operator-crds`) covers CRD resources
  only. Namespaced resources (pods, events, leases) sit in namespace-scoped Role
  (`*-operator-ns`). Never merge them back into single ClusterRole.
- **VmafxTenant in ClusterRole**: `vmafxtenants` rule must remain in CRD
  ClusterRole; removing it causes controller-runtime watch setup to silently fail.
- **PDB template**: `templates/pdb.yaml` uses `policy/v1` (requires k8s >= 1.21).
  To support older clusters, add `capabilities.apiVersions.has` guard.
- **Metrics NetworkPolicy**: `networkPolicy.allow.nodeMetrics` must remain enumerated
  in schema. Default `fromPodSelector: {}` allows any in-namespace pod to
  scrape; production clusters should narrow this to Prometheus pod selector.

## Invariants (ADR-1094)

- **node Deployment strategy**: `templates/node.yaml` must always have explicit
  `spec.strategy` block (sourced from `node.strategy`). Without it, Kubernetes defaults
  to 25%/25%, evicting GPU pods before replacements ready, dropping in-flight
  scoring jobs. Never remove `strategy:` block from node Deployment.
- **node probes use tcpSocket, not httpGet**: vmafx-node binary exposes only gRPC
  server. No HTTP listener exists. Both liveness and readiness probes must use
  `tcpSocket` on `port: grpc` (`node.grpcPort` value, default 50052). If future
  PR adds HTTP metrics/health endpoint to binary, probes may upgrade to
  `httpGet` — only after verifying endpoint live in container image.
- **terminationGracePeriodSeconds must be set on all pod specs**: all workload templates
  (`deployment.yaml`, `statefulset.yaml`, `node.yaml`) set `terminationGracePeriodSeconds`
  from `.Values.terminationGracePeriodSeconds`. Any new workload template must do same.
  Kubernetes 30 s default is insufficient for GPU scoring jobs.
- **PDB default is maxUnavailable, not minAvailable**: chart default for PDB =
  `maxUnavailable: 1`. `minAvailable` = opt-in for operators needing hard
  lower-bound on capacity (requires `replicaCount >= 2`). Never change default back
  to `minAvailable: 1` — permanently blocks node drain on single-replica deployments.
- **node Service name is `vmafx-node` (gRPC port)**: Service renamed from
  `vmafx-node-metrics` (phantom port 9090) to `vmafx-node` (gRPC port 50052) in
  ADR-1094. Any external tooling (NetworkPolicy selectors, ServiceMonitors)
  referencing old name must be updated.

## Operator env-only contract (ADR-1119 / ADR-1129)

`cmd/vmafx-operator` accepts only `--version`; runtime configuration = env-only.
`templates/operator-deployment.yaml` must therefore use compound-key env vars
`VMAFX_OPERATOR_METRICS_ADDR=:8080`,
`VMAFX_OPERATOR_HEALTH_PROBE_ADDR=:8081`, and
`VMAFX_OPERATOR_LEADER_ELECTION`, plus shared `VMAFX_LOG_LEVEL`. Never
restore removed pre-fx CLI flags. Named container ports and probes must
remain aligned with metrics `8080` and health/readiness `8081`.

## Server component selector

Main HTTP Service selects `app.kubernetes.io/component: server` in addition
to release name/instance. Every server workload Pod template (Deployment,
StatefulSet, Job) must carry that label; operator and node use their own
component labels. Without component discriminator, enabling operator
adds its metrics port 8080 as endpoint behind scoring Service, producing
nondeterministic HTTP 404 responses. Keep headless Service, main PDB, HTTP
NetworkPolicy, and ServiceMonitor selectors aligned with server label.

Never add label to existing Deployment/StatefulSet `spec.selector` in
patch release: those fields are immutable for installed workloads. Pod
template label plus consumer selectors provides upgrade-safe routing isolation.

## References

- [ADR-0699](../../../docs/adr/0699-vmafx-helm-chart-k8s.md) — original chart ADR
- [ADR-0930](../../../docs/adr/0930-helm-networkpolicy-pss.md) — PSS + NetworkPolicy
- [ADR-0969](../../../docs/adr/0969-helm-seccomp-default-plus-node-image-helper.md) — seccompProfile default + node image helper fix
- [ADR-1047](../../../docs/adr/1047-helm-schema-bug-fixes.md) — R9 schema correctness fixes
- [ADR-1058](../../../docs/adr/1058-helm-chart-security-hardening.md) — PDB, RBAC split, metrics NetworkPolicy, schema tightening
- [ADR-1074](../../../docs/adr/1074-helm-values-completeness.md) — nameOverride/fullnameOverride, statePVCSize, node.metricsPort, extraPorts items schema
- [ADR-1094](../../../docs/adr/1094-helm-rolling-update-correctness.md) — rolling-update strategy, probe fix, PDB default, grace period
- [ADR-1119](../../../docs/adr/1119-golusoris-go-framework-adoption.md) — env-only fx migration
- [ADR-1129](../../../docs/adr/1129-release-container-runtime-alignment.md) — release image/runtime alignment

<!-- markdownlint-disable MD013 -->
# AGENTS.md — deploy/helm/vmafx/

This directory contains the VMAFX Helm chart. Notes for agents working here.

## Invariants

### podSecurityContext must always include seccompProfile (ADR-0969)

Every workload template (`deployment.yaml`, `job.yaml`, `statefulset.yaml`,
`node-deployment.yaml`, `operator-deployment.yaml`) renders its pod-level
security context from `.Values.podSecurityContext`. The Kubernetes PSA
"restricted" admission profile requires `seccompProfile.type` be set to
`RuntimeDefault` or `Localhost`. The default value is set in `values.yaml`:

```yaml
podSecurityContext:
  ...
  seccompProfile:
    type: RuntimeDefault
```

**Any new template that renders a pod spec must inherit `podSecurityContext`
verbatim** — do not hand-roll a partial security context that omits
`seccompProfile`. See ADR-0969 and ADR-0930.

### Node-worker image must use the vmafx.nodeImage helper (ADR-0969)

`templates/node-deployment.yaml` renders the node container image via:

```yaml
image: {{ include "vmafx.nodeImage" . }}
```

The helper (`templates/_helpers.tpl:145-149`) defaults the repository to
`<image.repository>-node` when `node.image.repository` is empty. Do not
replace this with an inline expression — the empty-repository default is
the load-bearing path for most installations.

### ADR-0930 follow-up

PR #439 (ADR-0930) will change `runAsUser`/`runAsGroup` from `65534` to
`65532` (distroless nonroot per ADR-0878) and add container-scope
`seccompProfile`. When that PR merges, update this note to reflect the
final UID and that container-scope seccompProfile is also set.

## Invariants (ADR-1047)

- `storage` must remain in `values.yaml` with `mode: "http-serve"` as the default;
  the schema defines the key as non-required but `additionalProperties: false` means
  any user-supplied `storage.*` key must match the schema definition.
- `gpu.count` minimum is 1; do not lower it back to 0 — 0 GPUs with a vendor device
  plugin is a silent no-op.
- `auth` and `otelCollector` use `additionalProperties: true` in the schema
  intentionally — their sub-keys are user-extensible (per-tenant oidc/rbac configs,
  arbitrary otel exporter blocks).
- `networkPolicy.allow` uses `additionalProperties: false` (changed in ADR-1058) —
  any new allow-rule must be enumerated in `values.schema.json` under the `allow`
  object properties. Do not revert this to `additionalProperties: true` without a
  corresponding ADR.

## Invariants (ADR-1058)

- **RBAC split**: the operator ClusterRole (`*-operator-crds`) covers CRD resources
  only. Namespaced resources (pods, events, leases) are in the namespace-scoped Role
  (`*-operator-ns`). Do not merge them back into a single ClusterRole.
- **VmafxTenant in ClusterRole**: the `vmafxtenants` rule must remain in the CRD
  ClusterRole; removing it causes the controller-runtime watch setup to silently fail.
- **PDB template**: `templates/pdb.yaml` uses `policy/v1` (requires k8s >= 1.21).
  If you need to support older clusters, add a `capabilities.apiVersions.has` guard.
- **Metrics NetworkPolicy**: `networkPolicy.allow.nodeMetrics` must remain enumerated
  in the schema. Its default `fromPodSelector: {}` allows any in-namespace pod to
  scrape; production clusters should narrow this to the Prometheus pod selector.

## References

- [ADR-0699](../../../docs/adr/0699-vmafx-helm-chart-k8s.md) — original chart ADR
- [ADR-0930](../../../docs/adr/0930-helm-networkpolicy-pss.md) — PSS + NetworkPolicy
- [ADR-0969](../../../docs/adr/0969-helm-seccomp-default-plus-node-image-helper.md) — seccompProfile default + node image helper fix
- [ADR-1047](../../../docs/adr/1047-helm-schema-bug-fixes.md) — R9 schema correctness fixes
- [ADR-1058](../../../docs/adr/1058-helm-chart-security-hardening.md) — PDB, RBAC split, metrics NetworkPolicy, schema tightening

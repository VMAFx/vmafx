<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1058: Helm chart security hardening — PDB, RBAC split, metrics NetworkPolicy, schema tightening

- **Status**: Accepted
- **Date**: 2026-06-06
- **Deciders**: Lusoris
- **Tags**: `helm`, `k8s`, `rbac`, `security`, `networkpolicy`

## Context

A focused audit of `deploy/helm/vmafx/` identified six actionable gaps:

1. **No PodDisruptionBudget** — no `pdb.yaml` template exists. When
   `replicaCount >= 2` (HA mode), Kubernetes can drain all pods simultaneously
   during a node drain or cluster upgrade, causing a full service outage.

2. **RBAC over-permissive** — `operator-rbac.yaml` issued a single
   `ClusterRole` + `ClusterRoleBinding` covering both CRD access (legitimately
   cluster-scoped) and namespaced resources (pods, events, leases). The
   `ClusterRole` granted the operator `create/update/patch/delete` on `pods`
   cluster-wide, violating the principle of least privilege on multi-tenant
   clusters.

3. **VmafxTenant missing from operator RBAC** — the operator reconciler
   watches `VmafxTenant` CRs to enforce per-tenant auth policy (ADR-0794), but
   the `vmafxtenants` resource was absent from the RBAC rules, causing the
   controller-runtime watch setup to silently fail at startup.

4. **NetworkPolicy Prometheus gap** — the `allow-http-ingress` policy opens
   only `service.targetPort` (8080). The vmafx-node metrics port (9090) had no
   allow-rule, so Prometheus scraping was silently blocked under
   `networkPolicy.enabled=true`.

5. **`values.schema.json` `networkPolicy.allow` uses `additionalProperties: true`**
   — typos in sub-keys (e.g. `enable` instead of `enabled`,
   `controllerToNodes` instead of `controllerToNode`) were silently accepted,
   defeating the purpose of the schema gate.

6. **No `podDisruptionBudget` in schema** — after adding the PDB values key,
   the schema must be updated to validate it.

## Decision

Fix all six gaps in a single PR:

1. Add `deploy/helm/vmafx/templates/pdb.yaml` with `policy/v1
   PodDisruptionBudget` resources for the controller, node pool, and operator.
   Add `podDisruptionBudget.enabled/minAvailable/maxUnavailable` to
   `values.yaml` (disabled by default for single-replica dev deployments).

2. Split the operator `ClusterRole` into:
   - `ClusterRole` + `ClusterRoleBinding` for CRD access only
     (`vmafxjobs`, `vmafxnodes`, `vmafxmodeltrainings`, `vmafxtenants`).
   - `Role` + `RoleBinding` (namespace-scoped) for pods, events, and
     leader-election leases in the release namespace.

3. Add `vmafxtenants: [get, list, watch]` + `vmafxtenants/status:
   [get, update, patch]` to the CRD ClusterRole (fixing the silent watch
   failure).

4. Add `allow-node-metrics-ingress` NetworkPolicy with an ingress rule on port
   9090 for vmafx-node pods, guarded by
   `networkPolicy.allow.nodeMetrics.enabled` (default `true` when networkPolicy
   is enabled). `fromPodSelector` allows narrowing the scraper identity.

5. Change `networkPolicy.allow` schema from `additionalProperties: true` to
   `additionalProperties: false` with exhaustively enumerated sub-keys
   (`controllerToNode`, `nodeEgressObjectStore`, `operatorToApiserver`,
   `nodeMetrics`, `dns`).

6. Add `podDisruptionBudget` property to `values.schema.json` with
   `additionalProperties: false`, validating `enabled`, `minAvailable`, and
   `maxUnavailable` (both accept integer or percentage string).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep single ClusterRole, add namespace filter in rules | Simpler manifest | Kubernetes RBAC has no namespace filter on ClusterRole rules — the filter is only on RoleBindings | Not valid; only a Role scoped to a namespace achieves the required restriction |
| `maxUnavailable: 1` as PDB default | More permissive, survives single pod loss | On a 2-replica deployment `minAvailable: 1` and `maxUnavailable: 1` are equivalent; `minAvailable` is more intuitive | Chose `minAvailable: 1` as default; operators can switch via values |
| Leave `networkPolicy.allow` as `additionalProperties: true` | Lower maintenance burden | Silently swallows typos — undermines the schema gate | Rejected; the exhaustive enumeration is already small and stable |

## Consequences

- **Positive**: PDB prevents full outage during voluntary disruptions in HA
  mode. RBAC now follows least-privilege. Prometheus scraping works under
  default-deny NetworkPolicy. Schema catches `networkPolicy.allow` typos.
- **Negative**: The RBAC split introduces a second binding object per
  `operator.enabled=true` install; `helm diff` will show the rename from
  `*-operator-role` to `*-operator-crds` / `*-operator-ns`. Operators must
  run `helm upgrade` (not in-place patch) to pick up the new ClusterRole name.
- **Neutral / follow-ups**: `podDisruptionBudget.enabled` defaults to `false`,
  so existing single-replica installs are unaffected. Follow-up: document the
  recommended PDB settings for HA deployments in
  `docs/development/k8s-deployment.md`.

## References

- Helm chart deep-audit (2026-06-06 parallel-push agent).
- ADR-0714: vmafx-operator kubebuilder skeleton.
- ADR-0794: multi-tenant auth gateway (VmafxTenant CRs).
- ADR-0930: Helm NetworkPolicy + Pod Security Standards baseline.
- ADR-1047: Helm chart schema and values.yaml correctness fixes (R9 batch).
- `deploy/helm/vmafx/templates/operator-rbac.yaml`
- `deploy/helm/vmafx/templates/pdb.yaml`
- `deploy/helm/vmafx/templates/networkpolicy.yaml`
- `deploy/helm/vmafx/values.yaml`
- `deploy/helm/vmafx/values.schema.json`

# Research digest: Helm chart security hardening (ADR-1058)

**Date:** 2026-06-06
**Scope:** `deploy/helm/vmafx/` — PDB, RBAC, NetworkPolicy, schema

## Findings

### 1. PodDisruptionBudget

`policy/v1 PodDisruptionBudget` is stable since Kubernetes 1.21 (GA).
The older `policy/v1beta1` API was removed in 1.25. The chart's minimum
supported Kubernetes version is 1.21+ (per Chart.yaml `kubeVersion`), so
`policy/v1` is the correct API version. A PDB does nothing for single-replica
deployments (a `minAvailable: 1` PDB on a 1-replica Deployment prevents ALL
voluntary disruptions); for that reason the default is `enabled: false`.
Operators running HA mode (replicaCount >= 2) should enable the PDB.

### 2. RBAC split: ClusterRole vs Role

Kubernetes RBAC scoping rules:

- **ClusterRole** + **ClusterRoleBinding** = cluster-wide access (reads/writes
  any instance of the resource, in any namespace).
- **ClusterRole** + **RoleBinding** (namespace-scoped) = access limited to the
  RoleBinding's namespace, but only for namespaced resources.
- **Role** + **RoleBinding** = access limited to the Role's namespace.

CRD objects (vmafxjobs, vmafxnodes, etc.) are cluster-scoped resources; a
ClusterRole is required to watch them across namespaces. However, `pods`,
`events`, and `leases` are namespaced resources. Granting them in a
ClusterRole + ClusterRoleBinding means the operator can read/delete pods in
any namespace — a violation of least privilege on multi-tenant clusters.

The correct split: ClusterRole for CRDs, Role for namespaced resources.

### 3. VmafxTenant RBAC

The `tenant-crd-config.yaml` template creates `VmafxTenant` CRs via Helm.
The operator reconciler (ADR-0794) watches these CRs to enforce per-tenant
auth policy. Without a `vmafxtenants: [get, list, watch]` rule in the
ClusterRole, `controller-runtime`'s `cache.New()` fails the informer setup
silently — the watch succeeds but returns no objects, and the reconciler
appears to work (no crash) but does not enforce tenant isolation.

### 4. NetworkPolicy metrics scraping

The vmafx-node `metrics` service (port 9090) is exposed via
`templates/node.yaml` and the `servicemonitor.yaml` ServiceMonitor. Under
the default-deny NetworkPolicy, the Prometheus operator's pod cannot reach
port 9090 on node pods — the scrape silently fails (connection refused) with
no error from the node. The fix adds an ingress allow-rule on port 9090,
gated by `networkPolicy.allow.nodeMetrics.enabled`.

### 5. Schema tightening for `networkPolicy.allow`

`additionalProperties: true` on `networkPolicy.allow` means a typo like
`controllerToNodes: enabled: true` (plural `Nodes`) passes `helm lint` and
produces a chart that silently does not open the gRPC allow-rule. Switching
to `additionalProperties: false` with an exhaustive enumeration of the five
allow-rule keys (`controllerToNode`, `nodeEgressObjectStore`,
`operatorToApiserver`, `nodeMetrics`, `dns`) catches this class of typo at
`helm lint` / `helm install --dry-run` time.

## Sources

- Kubernetes RBAC documentation: [kubernetes.io/docs/reference/access-authn-authz/rbac/](https://kubernetes.io/docs/reference/access-authn-authz/rbac/)
- PodDisruptionBudget API: [kubernetes.io/docs/tasks/run-application/configure-pdb/](https://kubernetes.io/docs/tasks/run-application/configure-pdb/)
- controller-runtime informer setup behaviour (observed from operator logs in staging).

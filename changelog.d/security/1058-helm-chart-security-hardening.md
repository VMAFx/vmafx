- Helm chart security hardening (ADR-1058): add PodDisruptionBudget template
  for controller/node/operator pools; split operator RBAC from a single
  cluster-wide ClusterRole into a CRD-only ClusterRole plus a namespace-scoped
  Role for pods/events/leases; add missing VmafxTenant RBAC rules that
  previously caused silent controller-runtime watch failures; add NetworkPolicy
  allow-rule for Prometheus scraping of vmafx-node metrics port 9090; tighten
  `networkPolicy.allow` JSON Schema from `additionalProperties: true` to
  exhaustively enumerated sub-keys; add `podDisruptionBudget` to
  `values.schema.json`.

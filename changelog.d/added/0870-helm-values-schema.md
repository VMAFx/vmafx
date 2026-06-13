- **`deploy/helm/vmafx/values.schema.json` for install-time
  validation
  ([ADR-0870](../docs/adr/0870-helm-values-schema-and-container-rebuild-audit.md)).**
  New JSON Schema (Draft 2020-12) consulted automatically by
  `helm install` / `helm upgrade` / `helm lint --strict`. Enforces
  `enum` constraints on the three load-bearing fields: `workload`
  ∈ {`Deployment`, `Job`, `StatefulSet`}, `gpu.vendor`
  ∈ {`nvidia`, `amd`, `intel`, `cpu`}, `storage.mode`
  ∈ {`http-serve`, `rclone`}. Also enforces `image.pullPolicy`,
  `service.type`, `persistence.accessMode`, `operator.logLevel`,
  `statefulSet.podManagementPolicy`, and
  `monitoring.serviceMonitor.scheme` enums. Uses
  `additionalProperties: false` on every typed sub-object so
  sibling-key typos (`replicaCounts`, `repostiory`, `maxSurg`) fail
  fast with `values don't meet the specifications of the schema(s)`
  before any manifest renders, instead of silently dropping into an
  unused branch. Pass-through structures (`affinity`, `tolerations`,
  `nodeSelector`, `podSecurityContext`, `securityContext`,
  `livenessProbe`, `readinessProbe`, `env`, `envFrom`,
  `podAnnotations`, `config`, `topologySpreadConstraints`) remain
  generic `object` / `array` so they continue to track upstream
  Kubernetes API additions verbatim.

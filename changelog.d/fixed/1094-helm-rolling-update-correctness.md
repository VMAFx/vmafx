- `deploy/helm/vmafx/templates/node.yaml`: add explicit `spec.strategy`
  (`maxUnavailable: 0`, `maxSurge: 1`) to the vmafx-node worker Deployment.
  Without this, Kubernetes defaulted to 25%/25%, evicting GPU pods before
  replacements were ready and silently dropping in-flight scoring jobs (ADR-1094).
- `deploy/helm/vmafx/templates/node.yaml`: replace `httpGet` liveness and
  readiness probes on container port 9090 (`/metrics`) with `tcpSocket` probes
  on the gRPC port (default 50052, configurable via `node.grpcPort`). The node
  binary exposes no HTTP listener; the old probes returned `ECONNREFUSED` on
  every poll, leaving all node pods permanently unready (ADR-1094).
- `deploy/helm/vmafx/templates/node.yaml`: rename `vmafx-node-metrics` Service
  (phantom port 9090) to `vmafx-node` and wire it to the actual gRPC port.
- `deploy/helm/vmafx/templates/deployment.yaml`,
  `deploy/helm/vmafx/templates/statefulset.yaml`,
  `deploy/helm/vmafx/templates/node.yaml`: add `terminationGracePeriodSeconds`
  (default 60 s, configurable) to all pod specs. The Kubernetes 30 s default
  is insufficient for GPU scoring passes that regularly exceed 30 s per segment
  (ADR-1094).
- `deploy/helm/vmafx/templates/pdb.yaml`, `deploy/helm/vmafx/values.yaml`:
  change PDB default from `minAvailable: 1` to `maxUnavailable: 1`. With the
  chart's single-replica default, `minAvailable: 1` permanently blocked node
  drain operations when `podDisruptionBudget.enabled: true` was set (ADR-1094).
- `deploy/helm/vmafx/values.yaml`: add `node.strategy`, `node.grpcPort`,
  `terminationGracePeriodSeconds`; make `statefulSet.updateStrategy.rollingUpdate`
  explicit (`maxUnavailable: 1`, `partition: 0`).
- `deploy/helm/vmafx/values.schema.json`: add schema entries for the three new
  top-level and node-scoped keys above.

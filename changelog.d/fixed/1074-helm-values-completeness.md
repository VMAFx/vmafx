## Helm values completeness (ADR-1074)

- **`nameOverride` / `fullnameOverride` now accepted by `helm lint`**: both keys
  were read by `_helpers.tpl` but absent from `values.yaml` and the strict
  `additionalProperties: false` root schema, causing immediate validation failures.
- **StatefulSet MCP-state PVC size now configurable** via `statefulSet.statePVCSize`
  (default: `1Gi`); previously hardcoded in the template.
- **Node metrics port now configurable** via `node.metricsPort` (default: `9090`);
  the hardcoded value appeared in three template locations (node Deployment
  containerPort, node-metrics Service port, NetworkPolicy allow rule) and is now
  derived from a single values key.
- **`service.extraPorts` items schema added**: malformed port objects (missing `name`,
  non-integer `port`, invalid `protocol`) now fail `helm lint` instead of being
  silently accepted and rejected later by the Kubernetes apiserver.

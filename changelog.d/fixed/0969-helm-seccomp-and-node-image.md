- Helm chart: add `seccompProfile.type: RuntimeDefault` to the shared
  `podSecurityContext` default so all VMAFX workloads pass the Kubernetes
  PSA "restricted" admission profile without per-install overrides (B.1,
  ADR-0969). Fix the node-worker `Deployment` image reference to use the
  `vmafx.nodeImage` named helper, which correctly falls back to
  `<image.repository>-node` when `node.image.repository` is empty —
  previously rendered as `:3.0.0` (invalid reference, `ImagePullBackOff`)
  with default values (B.3, ADR-0969).

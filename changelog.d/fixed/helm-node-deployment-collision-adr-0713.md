- Helm chart: removed the duplicate `templates/node-deployment.yaml`
  Deployment that collided with `templates/node.yaml` (both rendered
  a Deployment named `{{ include "vmafx.fullname" . }}-node` in the
  same namespace under `.Values.node.enabled=true`, blocking
  `helm install` with a duplicate-resource error and making
  Phase 4b distributed scoring (ADR-0709 / ADR-0713) uninstallable).
  The richer `node.yaml` is kept — it carries liveness/readiness
  probes, GPU resource injection via `vmafx.gpuResourceKey`, the
  metrics port + Service, and the canonical `VMAFX_NODE_ID` env
  var (per ADR-0713). The rclone Secret mount, models PVC mount,
  and `VMAFX_STORAGE_MODE` / `VMAFX_RCLONE_CONFIG` /
  `VMAFX_VMAF_BINARY` / `VMAFX_MODEL_DIR` env vars from the deleted
  template have been folded into `node.yaml` so the ADR-0719 rclone
  integration remains intact.

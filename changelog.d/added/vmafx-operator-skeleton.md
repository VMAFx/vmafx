# vmafx-operator kubebuilder skeleton + CRDs (ADR-0714)

Added the vmafx-operator — a Kubernetes Operator built with kubebuilder v4 /
controller-runtime v0.19+.  Stage 1 ships the skeleton:

- **3 CRDs** in API group `vmafx.dev/v1`:
  - `VmafxJob` (`vmjob`) — one reference/distorted video-quality scoring job
  - `VmafxNode` (`vmnode`) — a GPU compute node
  - `VmafxModelTraining` (`vmtrain`) — online SGD-EMA sidecar training run
- **Stub reconcilers** for each CRD (Phase initialisation + health polling).
- **Helm integration**: `deploy/helm/vmafx/crds/` auto-installs CRDs on first
  `helm install`; `operator.enabled=true` deploys the operator Deployment + RBAC.
- **envtest suite**: verifies CRD installation + Stage 1 reconcile behaviour.
- Operator binary at `cmd/vmafx-operator/`.

Full Pod-lifecycle reconcilers and SGD-EMA training loop ship in Stage 2 PRs.

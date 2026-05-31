<!-- markdownlint-disable MD013 MD060 -->
# ADR-0714: vmafx-operator kubebuilder skeleton + CRDs

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: Lusoris
- **Tags**: `go`, `k8s`, `operator`, `crd`, `controller-runtime`, `phase4b`, `fork-local`

## Context

Phase 4b (ADR-0709) established a controller/node/operator architecture for
distributed VMAFX scoring.  ADR-0711 shipped the vmafx-controller with its
job queue, node registry, and scheduler.  The next layer is a Kubernetes
Operator that watches custom resources — `VmafxJob`, `VmafxNode`,
`VmafxModelTraining` — and reconciles Pods + status subresources.

Without CRDs and a running operator, the k8s-native workflow described in
ADR-0701 has no declarative surface: users cannot submit jobs via `kubectl
apply -f job.yaml` or query cluster state via `kubectl get vmafxjobs`.

Stage 1 (this ADR) delivers the skeleton: API types, CRD manifests, stub
reconcilers, Helm integration, and envtest coverage.  The full Pod-lifecycle
reconcilers, rolling upgrade strategy, and metrics emission are Stage 2+ PRs.

## Decision

The operator workspace is structured as follows.

### API group

`vmafx.dev/v1` — consistent with the Phase 4b architecture document.
Three CRDs are registered:

| Kind | Short name | Scope | Purpose |
|---|---|---|---|
| `VmafxJob` | `vmjob` | Namespaced | One reference↔distorted scoring job |
| `VmafxNode` | `vmnode` | Namespaced | A compute node offering GPU capacity |
| `VmafxModelTraining` | `vmtrain` | Namespaced | Online SGD-EMA sidecar training run |

### Directory layout

```text
api/vmafx/v1/                          # CRD Go types + deepcopy
cmd/vmafx-operator/
  main.go                              # entry point; manager setup
  internal/controller/
    vmafxjob_controller.go             # VmafxJob reconciler stub
    vmafxnode_controller.go            # VmafxNode reconciler stub
    vmafxmodeltraining_controller.go   # VmafxModelTraining reconciler stub
    suite_test.go                      # envtest bootstrap
    *_controller_test.go               # per-CRD envtest tests
config/
  crd/bases/                           # hand-authored CRD YAMLs
  rbac/role.yaml                       # ClusterRole
deploy/helm/vmafx/
  crds/                                # CRD YAMLs for Helm
  templates/operator-deployment.yaml
  templates/operator-rbac.yaml
  values.yaml                          # operator.* section added
```

### Stage 1 reconciler behaviour

**VmafxJob**: if Phase is empty, set to `Pending`; if AssignedNode is set
and Phase is `Pending`, advance to `Running`.  No Pod creation in Stage 1.

**VmafxNode**: every 30 seconds, probe `http://vmafx-controller.<ns>.svc.cluster.local:8080/healthz`;
update `Healthy` and `LastHeartbeat` in the status subresource.

**VmafxModelTraining**: set Phase to `Initializing` if empty; log and
requeue every 60 seconds.  Full SGD-EMA loop is Stage 2.

### Dependencies added

- `sigs.k8s.io/controller-runtime` v0.19.4
- `k8s.io/apimachinery`, `k8s.io/client-go` (pulled transitively)
- `go.uber.org/zap` (already pulled by controller-runtime; now direct for main)
- `github.com/onsi/ginkgo/v2`, `github.com/onsi/gomega` (tests)

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Run kubebuilder init/create-api | Standard scaffold; familiar to kubebuilder users | kubebuilder binary not available on host; scaffold adds boilerplate Makefiles + PROJECT files that conflict with our Meson workflow | Equivalent output produced manually; controller-gen codegen deferred to Stage 2 CI job |
| Single monolithic reconciler file | Fewer files | Impedes parallel Stage 2 development across CRD types | Three separate files match the standard kubebuilder pattern |
| operator-sdk instead of kubebuilder | Ansible/Helm operator flavours available | Larger toolchain dependency; kubebuilder/controller-runtime are the upstream primitives anyway | kubebuilder / controller-runtime chosen per ADR-0709 |
| Separate Go module for operator | Clean dependency isolation | Breaks the single-module workspace strategy established by ADR-0702 | Retained in the root module; `cmd/vmafx-operator/` prefix is sufficient isolation |

## Consequences

- **Positive**: `kubectl get vmafxjobs/vmafxnodes/vmafxmodeltrainings` works
  after `helm upgrade --set operator.enabled=true`.  CRDs auto-install from
  Helm's `crds/` directory on first install.
- **Positive**: envtest suite verifies CRD installation + reconcile trigger
  without a live cluster.
- **Negative**: `DeepCopyObject` implementations in
  `zz_generated_deepcopy.go` are hand-written; controller-gen integration
  (to auto-regenerate them) is a Stage 2 task.
- **Neutral**: Helm `operator.enabled` defaults to `false`; existing
  deployments are unaffected.  Set `--set operator.enabled=true` to opt in.

## References

- Parent: ADR-0709 (VMAFX Phase 4b distributed platform).
- Sibling controller: ADR-0711 (vmafx-controller Phase 4b.1).
- Research #30: sidecar online-learning pipeline (VmafxModelTraining design).
- `req`: Phase 4b.3 operator task brief — kubebuilder skeleton + stub reconcilers for
  VmafxJob, VmafxNode, VmafxModelTraining; Helm integration; envtest coverage.

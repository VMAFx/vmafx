# AGENTS.md — cmd/vmafx-operator

Parent: [../../AGENTS.md](../../AGENTS.md).

## Package role

vmafx-operator is a Kubernetes Operator built with kubebuilder v4 /
controller-runtime v0.19+.  It watches `VmafxJob`, `VmafxNode`, and
`VmafxModelTraining` CRDs in API group `vmafx.dev/v1` and reconciles
Pods + status subresources.

See [ADR-0714](../../docs/adr/0714-vmafx-operator-skeleton.md) and
[docs/development/operator.md](../../docs/development/operator.md).

## Rebase-sensitive invariants

1. **DeepCopyObject is hand-written.**  `api/vmafx/v1/zz_generated_deepcopy.go`
   is written by hand in Stage 1 (controller-gen codegen is a Stage 2 CI job).
   Do not delete or overwrite it without running `controller-gen object:headerFile=...`
   to regenerate it.

2. **CRD YAMLs live in two places.**  The canonical source is
   `config/crd/bases/`.  Helm ships copies in `deploy/helm/vmafx/crds/`.
   Keep both in sync whenever a CRD schema changes.

3. **Helm `operator.enabled` defaults to false.**  The operator Deployment
   and RBAC are gated by `operator.enabled`.  Changing the default to `true`
   would affect all existing `helm upgrade` runs.

4. **Reconciler stubs do not create Pods.**  Stage 1 reconcilers only
   update status subresources.  Pod lifecycle belongs in Stage 2.  Do not
   add Pod creation logic to Stage 1 reconciler files; open a new file instead.

5. **No shared state between reconcilers.**  Each reconciler has its own
   `client.Client` and `Scheme`.  Do not add package-level variables.

6. **Logger is `slog` via `logr.FromSlogHandler`, not zap.**  Despite
   kubebuilder's template defaulting to `sigs.k8s.io/controller-runtime/pkg/log/zap`,
   this operator deliberately uses the standard-library `log/slog` package
   so all 25 vmafx Go binaries log uniformly.  `main.go` installs the
   logger via `ctrl.SetLogger(logr.FromSlogHandler(slog.NewJSONHandler(...)))`
   and `internal/controller/suite_test.go` uses
   `slog.NewTextHandler(GinkgoWriter, ...)`.  Do not reintroduce
   `go.uber.org/zap` as a direct import when porting upstream
   kubebuilder template updates; keep the slog bridge.

## Test requirements

Run `go test ./cmd/vmafx-operator/internal/controller/...` with
`KUBEBUILDER_ASSETS` set (see `docs/development/operator.md#running-tests`).

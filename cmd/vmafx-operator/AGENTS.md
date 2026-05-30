# AGENTS.md — cmd/vmafx-operator

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

6. **envtest suite must remain skip-safe.**  `internal/controller/suite_test.go`
   has a top-of-`TestControllers` `t.Skip()` guard plus a nil-`testEnv`
   bailout in `AfterSuite` so the suite never panics when
   `KUBEBUILDER_ASSETS` is unset (PRs #330 / #341 / #362 all tripped the
   nil-pointer deref in `controlplane.(*APIServer).Stop` without these
   guards). The CI workflow (`go-ci.yml`) and `make setup-envtest` arrange
   the assets so the suite *does* run for real, but the skip path is the
   guardrail for ad-hoc `go test` invocations from a fresh checkout. Do
   not remove either guard without arranging an equivalent fail-loud
   mechanism elsewhere.

## Test requirements

Run `go test ./cmd/vmafx-operator/internal/controller/...` with
`KUBEBUILDER_ASSETS` set. The shortest path is `make setup-envtest`
followed by `eval $(make -s setup-envtest-env)`, then `go test`. See
`docs/development/operator.md#running-tests` for the manual variant.

## Added

- **`cmd/vmafx-operator/internal/controller`: functional test coverage uplift**
  (38.1% -> 84.1% statement coverage). Three new test files exercise the
  reconciliation branches the original Stage 1 suite skipped:
  - `vmafxnode_controller_test.go` — covers the VmafxNode reconciler end-to-end
    via an injected `http.Client` with a stub `RoundTripper` (no real network).
    Asserts Healthy=true on 200 OK, Healthy=false-but-still-requeued on
    transport error, and clean no-op on not-found.
  - `vmafxjob_controller_branch_test.go` — covers the not-found and terminal-
    phase branches (Stage 1 reconciler must not mutate a Succeeded job).
  - `vmafxmodeltraining_controller_branch_test.go` — covers the not-found and
    already-initialised branches.
  - `setup_with_manager_test.go` — verifies all three `SetupWithManager`
    surfaces register with a controller-runtime manager (previously 0%).
  Per-function coverage after this change: VmafxJob.Reconcile 81.0%,
  VmafxNode.Reconcile 85.7%, probeHealthz 83.3%, VmafxModelTraining.Reconcile
  84.6%, all three SetupWithManager surfaces at 100%. Builds on PR #396
  (`fix(ci,operator): install kubebuilder envtest binaries`).

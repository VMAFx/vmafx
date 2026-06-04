- **k8s operator resource-allocation fixes** (`cmd/vmafx-operator`):
  two per-call resource leaks corrected (ADR-1017, r5-scheduler-timer):
  - `VmafxNodeReconciler.SetupWithManager` and
    `VmafxModelTrainingReconciler.SetupWithManager` now initialise
    `r.HTTPClient` once so all Reconcile calls share a connection-pooled
    client instead of creating a new TCP connection per probe.
  - `getRemoteJob` in `VmafxJobReconciler`: removed `grpc.WithBlock()` so
    the reconciler goroutine is not frozen during TCP handshake; the `GetJob`
    RPC now uses `dialCtx` (with the `grpcDialTimeout` deadline) instead of
    the root context to prevent indefinitely-blocking RPCs.

- **k8s operator correctness fixes + coverage** (`cmd/vmafx-operator`, ADR-1069):
  - `VmafxNodeReconciler.Reconcile` no longer overwrites `status.lastHeartbeat`
    on every 30 s probe cycle.  The field is owned exclusively by the node agent
    via the controller's Heartbeat RPC; clobbering it defeated stale-threshold
    detection, causing dead nodes to bounce healthy/unhealthy every 30 s.
  - Three new `TestLastHeartbeat*` pure-Go regression tests (fake client, no
    envtest) pin the ownership invariant.
  - Five new `TestResolveControllerHTTPURL_*` unit tests cover all three code
    paths of `resolveControllerHTTPURL` (struct-field override, env-var override,
    default in-cluster DNS), closing a 0% coverage gap.
  - Three new `TestControllerJobID_*` unit tests verify the `ControllerJobID`
    round-trip and the two reconcile paths that activate once the external
    scheduler sets the field.
  - Envtest assertions in `vmafxnode_controller_test.go` updated to reflect
    correct LastHeartbeat ownership semantics (BeNil / Equal stale value instead
    of NotTo(BeNil) / ShouldNot(Equal)).

<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1069: Operator CRD status-schema gaps and VmafxNode LastHeartbeat ownership

- **Status**: Accepted
- **Date**: 2026-06-06
- **Deciders**: Lusoris
- **Tags**: `operator`, `crd`, `k8s`, `bug`

## Context

Two correctness bugs were identified in the vmafx-operator (helm + operator deep audit, R14):

### Bug 1: `controllerJobID` missing from VmafxJob CRD schema

`VmafxJobStatus.ControllerJobID` is declared in `api/vmafx/v1/vmafxjob_types.go` and
is the field the `VmafxJobReconciler` reads to determine whether a job has been accepted
by the vmafx-controller scheduler.  However, the field was absent from both
`config/crd/bases/vmafx.dev_vmafxjobs.yaml` and `deploy/helm/vmafx/crds/vmafx.dev_vmafxjobs.yaml`.

When the CRD OpenAPI schema does not include a field, the Kubernetes API server's
structural schema pruning silently drops the field on every write.  An external scheduler
writing `controllerJobID` to the status subresource would have the value stripped, causing
the reconciler to loop forever in the "waiting for scheduler" branch — visible only as a
perpetual `Pending` phase with no error in the logs.

### Bug 2: VmafxNode reconciler unconditionally overwrites `LastHeartbeat`

The `VmafxNodeReconciler.Reconcile` function performed `node.Status.LastHeartbeat = &nowMeta`
on every reconcile cycle, setting the field to the operator's local clock time.

`status.lastHeartbeat` is intended to be written exclusively by the node agent via the
vmafx-controller's Heartbeat RPC — the operator reads it to detect stale nodes (nodes
whose agent has stopped sending heartbeats).  Overwriting the field every 30 s caused:

1. The stale-threshold check (`nodeStaleThreshold = 60 s`) could never fire after the first
   detection cycle: after detecting stale, the operator reset `LastHeartbeat` to `now`,
   making the field appear fresh on the next reconcile.
2. The `Healthy` field would oscillate between `false` and `true` every 30 s for a node
   whose agent had stopped — one reconcile marks it unhealthy (stale), the next marks it
   healthy (fresh timestamp written last cycle, probe succeeds).

## Decision

### Fix 1

Add `controllerJobID` to the `status.properties` block of both CRD YAML files with
`type: string`, matching the Go struct definition.

### Fix 2

Remove the `node.Status.LastHeartbeat = &nowMeta` write from `VmafxNodeReconciler.Reconcile`.
The reconciler must only write `status.healthy`.  `status.lastHeartbeat` is written
exclusively by the node agent; the operator only reads it.

Update the controller's doc comment, the tests, and drop the now-unused `metav1` import.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add `x-kubernetes-preserve-unknown-fields: true` to the VmafxJob status schema | Simpler, no field listing | Opts out of structural schema for the entire status; loses all other validation | Not chosen — field-by-field schema is the correct pattern |
| Have the operator track its own probe time in a separate `lastProbeTime` field | Preserves both timestamps | Requires a new status field and CRD schema update | Out of scope; the key fix is stop clobbering the node-owned field |

## Consequences

- **Positive**: External schedulers can reliably set `controllerJobID`; the reconciler
  will correctly detect when the field is present and begin polling.
- **Positive**: `status.lastHeartbeat` now accurately reflects the node agent's last
  reported heartbeat, not the operator's probe time.  Stale detection fires and
  stays fired until the agent resumes.
- **Test impact**: Three tests in `vmafxnode_controller_test.go` were updated to reflect
  the correct ownership semantics: `LastHeartbeat` is nil for nodes that have never had
  an agent heartbeat, and unchanged after reconcile for nodes with a stale heartbeat.

## References

- R14 helm + operator deep audit (2026-06-06).
- `api/vmafx/v1/vmafxjob_types.go`, `config/crd/bases/vmafx.dev_vmafxjobs.yaml`,
  `deploy/helm/vmafx/crds/vmafx.dev_vmafxjobs.yaml`,
  `cmd/vmafx-operator/internal/controller/vmafxnode_controller.go`.
- ADR-0786: vmafx-operator Stage 2 (parent).
- ADR-0714: vmafx-operator kubebuilder skeleton + CRDs (grandparent).

<!-- markdownlint-disable MD013 -->
# ADR-0786: vmafx-operator Stage 2 — reconciler loops, webhook validation, per-controller RBAC

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: Lusoris
- **Tags**: `go`, `k8s`, `operator`, `crd`, `controller-runtime`, `phase4b`, `fork-local`

## Context

ADR-0714 delivered the vmafx-operator skeleton with stub reconcilers that only
initialised Phase fields and logged.  The operator was not yet useful: VmafxJob
status never updated beyond Pending, VmafxNode never detected stale nodes, and
VmafxModelTraining never emitted events.  Webhook admission validation was absent,
and the single combined ClusterRole granted more permissions than any individual
controller needed.

This ADR records the Stage 2 decisions that close those gaps:

1. **VmafxJob reconciler** polls the vmafx-controller's gRPC `GetJob` endpoint and
   maps `PENDING → Pending`, `RUNNING → Running`, `COMPLETED → Succeeded`,
   `FAILED / CANCELLED → Failed`.  The `ControllerJobID` field (added to
   `VmafxJobStatus`) is the bridge between the external scheduler and the reconciler.

2. **VmafxNode reconciler** introduces a 60-second stale-heartbeat threshold.
   A node whose `LastHeartbeat` is older than 60 s is marked `Healthy = false`
   regardless of the HTTP probe result, covering the case where the node's own
   Heartbeat RPC to the controller has silently stopped.

3. **VmafxModelTraining reconciler** polls a per-training sidecar HTTP
   `/status` endpoint and emits a `CheckpointWritten` Kubernetes event whenever
   `LastCheckpoint` advances.

4. **Webhook admission validation** (opt-in, `--webhooks-enabled` flag):
   - `VmafxJob`: `spec.reference` and `spec.distorted` must be valid rclone URIs
     (non-empty, `scheme://` form; accepts `file://`, `s3://`, `rclone://`,
     `gs://`, and any other `alphabet://` URI).
   - `VmafxNode`: `spec.gpuVendor` must be one of `nvidia`, `amd`, `intel`, `cpu`.

5. **Per-controller RBAC**: three separate ClusterRoles
   (`vmafx-operator-vmafxjob-role`, `vmafx-operator-vmafxnode-role`,
   `vmafx-operator-vmafxmodeltraining-role`) each grant only the verbs required by
   that controller.  The combined `role.yaml` from Stage 1 is preserved as a
   convenience aggregate for non-production use.

6. **envtest coverage** extended to 7 specs: VmafxJob (Pending on new object,
   stays Pending without ControllerJobID, no requeue on terminal),
   VmafxNode (healthy / unhealthy probe, stale-heartbeat gate),
   VmafxModelTraining (Initializing phase).  Webhook tests are pure unit tests
   (no live API server needed).

## Decision

### VmafxJob gRPC polling

The reconciler dials `vmafx-controller.<ns>.svc.cluster.local:9090` (override via
`VMAFX_CONTROLLER_GRPC_ADDR`) with a 5-second timeout and calls `GetJob`.  A new
`ControllerJobID` field on `VmafxJobStatus` is the scheduler-assigned UUID.  Until
the field is set, the reconciler requeues every 10 s without dialling.

The `Job` proto's hand-maintained generated stub gains a `FinalScore float64`
field (field 9) so the operator can propagate the aggregate VMAF score to
`VmafxJobStatus.Score` on completion.

### VmafxNode stale-heartbeat gate

`LastHeartbeat` on VmafxNode is written by the **operator** on every reconcile
(30 s probe interval).  A separate field `NodeLastSelfHeartbeat` (future) will
carry the node's own Heartbeat RPC timestamp; for Stage 2 the operator's probe
timestamp is used as a proxy.  Nodes whose stored `LastHeartbeat` is older than
60 s are marked unhealthy even if the HTTP probe would succeed — this handles
the case where the operator itself is the only thing still running.

### Webhook opt-in flag

Webhooks require a valid TLS certificate (cert-manager or a manual secret).
Running the operator without cert-manager is the common development path;
`--webhooks-enabled=false` (default) lets the operator start without TLS.

### Per-controller RBAC minimum permissions

| Controller | get/list/watch | update/patch | delete | create |
| --- | --- | --- | --- | --- |
| VmafxJob | vmafxjobs | vmafxjobs, vmafxjobs/status | — | — |
| VmafxNode | vmafxnodes | vmafxnodes, vmafxnodes/status | — | — |
| VmafxModelTraining | vmafxmodeltrainings | vmafxmodeltrainings, vmafxmodeltrainings/status | — | — |

All three also require `events: create, patch` (for event emission) and
`leases: *` (for leader election when enabled).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| HTTP polling instead of gRPC for VmafxJob | No gRPC dep; simpler | Controller's canonical API is gRPC; HTTP REST wrapper would need a new route | gRPC is already the canonical API (ADR-0711) |
| Separate heartbeat timestamp field on VmafxNode | Cleaner semantics | Requires a second controller write path; Stage 3 work | Deferred to Stage 3 |
| Always-on webhooks | Simpler startup | Breaks operator without cert-manager | Opt-in flag costs one flag and is more accessible |
| One aggregate ClusterRole (Stage 1 approach) | Fewer YAML files | Over-permissive; violates least-privilege | Kept as convenience alias; per-controller roles added |

## Consequences

- **Positive**: `kubectl get vmafxjobs` now shows live Phase reflecting the
  controller's actual job state.
- **Positive**: Stale VmafxNode objects are detectable within 90 s of the node
  going silent (one probe interval + stale threshold).
- **Positive**: `kubectl get events` shows `CheckpointWritten` events for each
  model checkpoint during training.
- **Positive**: Invalid rclone URIs and unknown GPU vendor strings are rejected at
  admission time (when webhooks are enabled).
- **Negative**: gRPC dial per reconcile is chatty; Stage 3 will introduce a pooled
  connection shared across reconciles.
- **Neutral**: `ControllerJobID` on VmafxJobStatus is set by the external scheduler
  (vmafx-controller), not by the operator; the operator is read-only for this field.

## References

- Parent: ADR-0714 (vmafx-operator skeleton + CRDs Stage 1).
- Sibling: ADR-0711 (vmafx-controller gRPC service definition).
- Parent platform: ADR-0709 (VMAFX Phase 4b distributed platform).
- `req`: Stage 2 handoff — implement VmafxJob gRPC poll, VmafxNode stale-heartbeat,
  VmafxModelTraining checkpoint events, webhook validation, per-controller RBAC,
  and envtest suite.

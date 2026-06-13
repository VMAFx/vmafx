### vmafx-operator Stage 2: live reconciler loops, webhook validation, per-controller RBAC

The vmafx-operator's three CRD reconcilers are now functional:

- **VmafxJob**: polls the vmafx-controller `GetJob` gRPC endpoint every 10 s and
  propagates `PENDING → Pending`, `RUNNING → Running`, `COMPLETED → Succeeded`,
  `FAILED/CANCELLED → Failed` into the CR's status subresource.  A new
  `controllerJobID` field on `VmafxJobStatus` is the scheduler-assigned bridge.

- **VmafxNode**: marks `Healthy = false` when the stored `LastHeartbeat` is older
  than 60 seconds, regardless of the HTTP probe result, preventing stale nodes
  from appearing healthy after a silent disconnect.

- **VmafxModelTraining**: polls the per-training sidecar `/status` endpoint and
  emits a `CheckpointWritten` Kubernetes event whenever `LastCheckpoint` advances.

**Webhook admission validation** (opt-in via `--webhooks-enabled`):
- `VmafxJob.spec.reference` and `spec.distorted` must be valid rclone URIs
  (`scheme://` form: `file://`, `s3://`, `rclone://`, `gs://`, etc.).
- `VmafxNode.spec.gpuVendor` must be one of `nvidia`, `amd`, `intel`, `cpu`.

**Per-controller RBAC**: three minimum-permission `ClusterRole` manifests under
`config/rbac/` replace the single over-permissive Stage 1 role.

**envtest suite**: 7 specs covering Pending initialisation, stale-heartbeat gate,
terminal no-requeue, healthy/unhealthy probe, and Initializing phase.

ADR-0786.

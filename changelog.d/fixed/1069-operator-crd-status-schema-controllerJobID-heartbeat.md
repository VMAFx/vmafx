## operator/crd: fix `controllerJobID` schema gap + VmafxNode `LastHeartbeat` ownership

**`controllerJobID` was missing from the VmafxJob CRD schema**, causing the Kubernetes
API server to silently prune the field on every status write. External schedulers writing
the field had it dropped, leaving the reconciler stuck in perpetual `Pending` (ADR-1069).

**VmafxNode reconciler unconditionally overwrote `status.lastHeartbeat`** with the
operator's probe time, preventing the stale-threshold check from firing after the first
detection cycle. Stale nodes bounced between healthy and unhealthy every 30 s. The field
is now treated as read-only by the operator; it is written exclusively by the node agent
(ADR-1069).

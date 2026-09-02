<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1094: Helm chart rolling-update correctness — node strategy, PDB default, probe fix, grace period

- **Status**: Accepted
- **Date**: 2026-06-07
- **Deciders**: Lusoris
- **Tags**: `helm`, `kubernetes`, `deploy`, `fork-local`

## Context

Four correctness gaps in `deploy/helm/vmafx/` were discovered during a targeted
rolling-update audit:

1. **`templates/node.yaml` — no `spec.strategy`.**  The vmafx-node worker
   Deployment had no explicit strategy block.  Kubernetes defaults to
   `RollingUpdate` with `maxUnavailable: 25%` and `maxSurge: 25%`, which means
   up to 25% of node pods can be evicted before their replacements are ready.
   For GPU-attached workers executing in-flight scoring jobs, this silently drops
   work (SIGTERM before any replacement starts serving).  The controller
   Deployment already had `maxUnavailable: 0` — the same treatment was missing
   from the node.

2. **`templates/node.yaml` — probes hit a phantom HTTP endpoint.**  Both
   `livenessProbe` and `readinessProbe` used `httpGet` on container port 9090
   (named `metrics`).  The vmafx-node binary (`cmd/vmafx-node/main.go`) exposes
   only a gRPC server (default `:50052`).  There is no HTTP listener on 9090.
   Every probe returned `ECONNREFUSED`, leaving all node pods permanently
   unready and never eligible to receive traffic.  Cluster operators enabling
   `node.enabled=true` saw Deployments stuck at 0/N ready replicas.

3. **`templates/pdb.yaml` — `minAvailable: 1` blocks single-replica drain.**
   The previous PDB default was `minAvailable: 1`.  With `replicaCount: 1`
   (the chart default), enabling the PDB permanently prevents voluntary
   disruptions: Kubernetes cannot satisfy `minAvailable: 1` while draining the
   node that hosts the only pod.  Node drains, cluster upgrades, and AKS/GKE
   node pool rotations all stall indefinitely.  The correct default for arbitrary
   replica counts is `maxUnavailable: 1` (allow one voluntary disruption at a
   time); `minAvailable` should be opt-in for operators who need a hard
   lower-bound on serving capacity.

4. **`terminationGracePeriodSeconds` absent from Deployment + StatefulSet.**
   Both workload templates relied on Kubernetes' 30-second default.  A full-feature
   VMAF score pass on a 1080p segment can take 15–40 s; vmaf-tune encode + score
   passes on long segments regularly exceed 30 s.  Mid-frame SIGKILL corrupts
   output JSON and forces a full re-run of the segment.  60 s is chosen as a
   conservative default that covers the vast majority of single-segment jobs;
   the value is configurable for operators running long CHUG extractions.

A secondary issue (StatefulSet `updateStrategy.rollingUpdate` implicit defaults)
was also addressed by making `maxUnavailable: 1` and `partition: 0` explicit,
removing silent reliance on Kubernetes internal defaults.

## Decision

We will apply the following changes to `deploy/helm/vmafx/`:

- **`templates/node.yaml`**: add `spec.strategy` sourced from
  `node.strategy` (default: `maxUnavailable: 0, maxSurge: 1`); replace
  `httpGet` probes on phantom port 9090 with `tcpSocket` probes on the gRPC
  port (default 50052, configurable via `node.grpcPort`); rename the
  `vmafx-node-metrics` Service to `vmafx-node` and wire it to the gRPC port
  instead of the phantom metrics port; add `terminationGracePeriodSeconds`
  from `terminationGracePeriodSeconds` (default 60 s).

- **`templates/pdb.yaml`**: flip default precedence from `minAvailable`-first
  to `maxUnavailable`-first, matching the new `values.yaml` default of
  `maxUnavailable: 1`.  `minAvailable` is still rendered when explicitly set.

- **`templates/deployment.yaml`** and **`templates/statefulset.yaml`**: add
  `terminationGracePeriodSeconds` from the shared `terminationGracePeriodSeconds`
  value (default 60 s).

- **`values.yaml`**: add `node.strategy`, `node.grpcPort`,
  `terminationGracePeriodSeconds`; change PDB default from `minAvailable: 1`
  to `maxUnavailable: 1`; add explicit `maxUnavailable` and `partition` to
  `statefulSet.updateStrategy`.

- **`values.schema.json`**: add schema entries for `terminationGracePeriodSeconds`,
  `node.strategy`, and `node.grpcPort`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add HTTP `/readyz` endpoint to vmafx-node binary | Real application-layer readiness | Requires Go binary change; out-of-scope for helm-only audit | Defer to a separate PR when node metrics endpoint is added |
| Use `grpc` probe type (k8s 1.24+) | Native gRPC health protocol | Requires implementing gRPC health service in the node binary | Defer to the same metrics endpoint follow-up |
| Keep `minAvailable: 1` as PDB default with a warning comment | No behaviour change | Still blocks single-replica drains when enabled | Incorrect; the default must not be a footgun |
| Set `terminationGracePeriodSeconds: 300` | Covers CHUG extractions | Too long for rolling upgrades on small clusters | Operators raising the value is the documented path; 60 s is the sensible default |

## Consequences

- **Positive**: node Deployments now become Ready (probes were always failing
  before this fix); rolling upgrades no longer SIGKILL in-flight scoring jobs;
  enabling PDB on single-replica dev clusters no longer permanently blocks drain
  operations; grace period is visible and configurable.
- **Negative**: the `vmafx-node-metrics` Service name changes to `vmafx-node`
  — operators who have hard-coded the old service name in external tooling must
  update.  The PDB default changes from `minAvailable` to `maxUnavailable`;
  operators who explicitly set `podDisruptionBudget.enabled: true` (non-default)
  and relied on `minAvailable: 1` semantics must explicitly set
  `podDisruptionBudget.minAvailable: 1` and remove `maxUnavailable`.
- **Neutral / follow-ups**: add a Prometheus metrics HTTP endpoint and
  `/readyz` handler to the vmafx-node binary so the probe can be upgraded
  from `tcpSocket` to `httpGet`; align NetworkPolicy `controllerToNode.nodePort`
  (currently 50051 in the docs) with the actual gRPC default (50052).

## References

- ADR-1058: Helm chart security hardening (PDB, RBAC, NetworkPolicy schema).
- ADR-0930: Helm NetworkPolicy + PSS hardening.
- ADR-0713: vmafx-node Go worker binary.
- Related: `deploy/helm/vmafx/templates/node.yaml`, `pdb.yaml`, `deployment.yaml`,
  `statefulset.yaml`, `values.yaml`, `values.schema.json`.
- Source: agent audit round 4 — rolling-update strategy correctness.

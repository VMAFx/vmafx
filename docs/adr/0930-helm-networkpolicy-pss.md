<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-0930: Helm chart NetworkPolicy default-deny + Pod Security Standards "restricted" baseline

- **Status**: Proposed
- **Date**: 2026-06-03
- **Deciders**: Lusoris
- **Tags**: `security`, `k8s`, `ci`, `build`, `github`

## Context

The vmafx Helm chart (`deploy/helm/vmafx/`, ADR-0699) shipped with minimal pod security
configuration. Default Kubernetes pod security contexts allow containers to run as root,
with writable root filesystems, and without seccomp profiles. This does not satisfy the
Kubernetes `pod-security.kubernetes.io/enforce=restricted` admission profile required
by most hardened cluster operators.

Additionally, the chart emitted no NetworkPolicy resources, leaving all cross-pod
traffic unrestricted. The Phase 4b platform (ADR-0709) introduces a controller → node
gRPC channel, a node → object-store HTTPS path, and operator → apiserver traffic, each
of which should be narrowly allowed with everything else denied by default.

The distroless `nonroot` uid used in the operator and node images (ADR-0878) is
65532, but the chart previously defaulted `podSecurityContext.runAsUser` to 65534
(the generic `nobody` uid). This drift caused file-ownership inconsistencies when
image-baked paths were accessed at runtime.

## Decision

We will update the Helm chart to: (1) align `podSecurityContext.runAsUser` to 65532
(distroless `nonroot`, per ADR-0878) across operator and node deployments; (2) set
`runAsNonRoot: true`, `allowPrivilegeEscalation: false`, `readOnlyRootFilesystem: true`,
and `seccompProfile.type: RuntimeDefault` as chart defaults, satisfying PSA
`restricted` out of the box; (3) ship `templates/networkpolicy.yaml` with a
default-deny baseline and narrow allow-rules (controller → node gRPC, node → object
store HTTPS, operator → apiserver, DNS, in-namespace HTTP ingress), gated by
`networkPolicy.enabled=false` so existing installs are unaffected by default.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| NetworkPolicy enabled by default | Immediately hardens all installs | Breaks installs on clusters without a NetworkPolicy-aware CNI (Flannel, older EKS); migration friction | Opt-in via `networkPolicy.enabled=true` is safer for a Helm library chart |
| BYO NetworkPolicy (not in chart) | Maximum flexibility; no chart coupling | Every operator must maintain their own policies; no reference policy | Poor DX; reference policies reduce misconfiguration risk |
| UID 65534 (generic `nobody`) | Standard on non-distroless images | Mismatches distroless baked paths; causes file-ownership bugs at runtime | ADR-0878 already established 65532 as the canonical uid |
| Inline security blocks per workload template | Explicit per-template; no values indirection | Duplication; values override becomes impossible without template changes | Values-driven blocks allow cluster admins to override per-deploy |
| Pod Security Policy (deprecated) | Pre-1.25 clusters | Removed in Kubernetes 1.25; no-op on modern clusters | Deprecated; PSA labels are the successor |
| `seccompProfile.type: Localhost` (custom profile) | Maximum control over syscall allowlist | Requires profile distribution to each node; operational overhead | RuntimeDefault covers the common case; Localhost can be a follow-up |

## Consequences

- **Positive**: Chart default render satisfies PSA `restricted` without manual
  overrides; NetworkPolicy opt-in provides a reference policy for hardened
  deployments; uid/GID alignment with distroless images eliminates file-ownership
  drift.
- **Negative**: `podSecurityContext.runAsUser` flip from 65534 → 65532 is a
  potentially breaking change for installs that hard-coded the old value (migration
  note in PR description and `NOTES.txt`).
- **Neutral / follow-ups**: When a dedicated `vmafx-controller` Service ships,
  tighten the `controller-to-node` NetworkPolicy selector from "any pod in namespace"
  to `component=controller`; track Kubernetes 1.31+ `appArmorProfile` for per-backend
  AppArmor; tighten `nodeEgressObjectStore.cidrs` in production overlays.

## References

- Open DRAFT PR: #439 (`feat(helm): NetworkPolicy default-deny + Pod Security Standards "restricted" baseline`).
- ADR-0699: vmafx Helm chart foundation.
- ADR-0709: Phase 4b distributed platform (defines the traffic matrix).
- ADR-0719: vmafx-node rclone integration (node → object store HTTPS path).
- ADR-0878: Trivy DS-0002 distroless `nonroot` uid 65532 baseline.

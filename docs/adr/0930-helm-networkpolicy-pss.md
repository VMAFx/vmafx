<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-0930: Ship NetworkPolicy default-deny + Pod Security Standards "restricted" in the VMAFX Helm chart

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: helm, kubernetes, security, networkpolicy, podsecurity, fork-local

## Context

The VMAFX Helm chart (`deploy/helm/vmafx/`) ships the controller, optional
`vmafx-node` worker Deployment, optional `vmafx-operator` Deployment, batch
`Job` form, and sticky `StatefulSet` form.  Before this ADR the chart shipped
a partial pod-security baseline:

- `podSecurityContext` set `runAsNonRoot: true` and `runAsUser: 65534` (the
  generic `nobody` UID), missing `seccompProfile`.
- `securityContext` set `allowPrivilegeEscalation: false`,
  `readOnlyRootFilesystem: true`, and dropped `ALL` capabilities, but did
  not set `runAsNonRoot`, `runAsUser`, or `seccompProfile` at the container
  level.
- `operator-deployment.yaml` and `tests/test-connection.yaml` hard-coded
  their own security blocks instead of inheriting from `.Values`, so any
  future change to the chart-wide defaults silently skipped them.
- No NetworkPolicies were shipped — the chart relied on the cluster's CNI
  to provide isolation, which is not the default on most managed control
  planes (GKE Autopilot is the notable exception).

PR #367 (ADR-0878) standardised every production VMAFX container image on
`gcr.io/distroless/cc-debian12`'s baked-in `nonroot` user (UID 65532).  The
chart's UID drifted from the image's UID, so the pod started as `nobody`
(65534) but the binary, its caches, and the model directory were owned by
the image's `nonroot` (65532).  Read-only mounts worked by luck; any
writable volume (`/tmp` `emptyDir`, output PVC, rclone cache) would write
files owned by 65534 that the image's tools could not later read back.

The Kubernetes [Pod Security Admission "restricted"
profile](https://kubernetes.io/docs/concepts/security/pod-security-admission/)
encodes the industry-standard hardening checklist (no privilege escalation,
no host namespaces, dropped capabilities, `RuntimeDefault` seccomp, ...).
Operators routinely label production namespaces with
`pod-security.kubernetes.io/enforce=restricted`; the chart must render pods
that pass that admission gate out of the box.

## Decision

Update `deploy/helm/vmafx/` so the chart's default render passes
`pod-security.kubernetes.io/enforce=restricted` admission, and add an
opt-in NetworkPolicy bundle that operators can flip on with a single
`--set` flag.

Concretely:

1. **UID alignment**: change `podSecurityContext.runAsUser` /
   `runAsGroup` / `fsGroup` from `65534` to `65532` (matches distroless
   `nonroot` from ADR-0878).
2. **Seccomp + container-level identity**: add
   `seccompProfile.type: RuntimeDefault` to both `podSecurityContext`
   and the container-level `securityContext`, and add `runAsNonRoot: true`
   / `runAsUser: 65532` to the container-level `securityContext` (PSA
   `restricted` checks both pod and container scope).
3. **Template centralisation**: refactor `operator-deployment.yaml` and
   `tests/test-connection.yaml` to inherit `podSecurityContext` and
   `securityContext` from `.Values`, eliminating the hard-coded drift.
4. **NetworkPolicy**: add `templates/networkpolicy.yaml` rendering a
   default-deny ingress + egress baseline plus five explicit allow-rules:
   in-namespace HTTP ingress, controller -> node gRPC, node -> object
   store HTTPS (with a `cidrs` + `except` matrix), operator -> apiserver,
   and DNS egress to `kube-system` / CoreDNS.  Gated by
   `networkPolicy.enabled: false` so the chart still installs cleanly on
   clusters without a NetworkPolicy-aware CNI.
5. **Operator UX**: extend `NOTES.txt` with the recommended `kubectl
   label namespace ... pod-security.kubernetes.io/enforce=restricted`
   command and a NetworkPolicy verification snippet, both conditioned on
   `networkPolicy.enabled` so the message is accurate.
6. **Documentation**: expand `docs/development/k8s-deployment.md` with a
   `## Pod security` table and a `## NetworkPolicy` matrix covering each
   policy's direction, peer, ports, and purpose, plus the override knobs
   in `values.yaml`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|--------|------|------|----------------|
| Ship NetworkPolicy enabled by default | Stronger default posture | Breaks installs on clusters without a NetworkPolicy controller (the manifests would render but be silently inert, and the user would not learn that until they tried to debug a connectivity issue).  Also stomps on operators who already manage policies via Cilium ClusterwideNetworkPolicy / Calico GlobalNetworkPolicy. | Opt-in `--set networkPolicy.enabled=true` keeps the default install lossless while making the hardened path one flag away. |
| Skip NetworkPolicy entirely; document "BYO CNI policy" | Zero chart maintenance | Pushes the work onto every operator; the documented flows (controller -> node, node -> object-store, operator -> apiserver) are stable enough that we can pre-canonicalise them. | Operators routinely ask for a NetworkPolicy baseline; not shipping one means each install re-invents the same five policies. |
| Use UID 65534 (`nobody`) everywhere | Smaller diff | Drifts from the distroless `nonroot` (65532) baked into every production image; writable volumes end up with mixed ownership. | UID 65532 matches ADR-0878 and is the well-known distroless convention. |
| Inline `securityContext` in each template (status quo) | Locality | Three templates already drifted (operator-deployment, test-connection, and the new networkpolicy).  Any chart-wide change has to be repeated N times. | Pull `podSecurityContext` / `securityContext` from `.Values` everywhere so a single edit to `values.yaml` rolls out to every workload kind. |
| Use a `PodSecurityPolicy` (PSP) | Cluster-side enforcement | PSP was removed in k8s 1.25; Pod Security Admission is the supported replacement. | Not viable on any supported cluster. |
| Use `seccompProfile.type: Localhost` with a custom profile | Tighter syscall filter than `RuntimeDefault` | Requires shipping the profile to every node out-of-band (DaemonSet, kubelet config); `RuntimeDefault` is the PSA `restricted` minimum and the industry-baseline ask. | `RuntimeDefault` is the right altitude for a chart default; users who want a stricter profile can override. |

## Consequences

- **Positive**:
  - The chart's default render passes `pod-security.kubernetes.io/enforce=restricted` admission.
  - File ownership inside the pod is now consistent (everything runs as
    65532) — no more silent `EACCES` on writable PVCs / `emptyDir` caches.
  - Operators get a turn-key NetworkPolicy bundle with a documented
    override surface; "node can't egress to my bucket" debugging now has
    a single grep target (`networkPolicy.allow.nodeEgressObjectStore.cidrs`).
  - `operator-deployment.yaml` and `tests/test-connection.yaml` no longer
    drift from the chart-wide defaults — one place to change every
    workload's hardening posture.

- **Negative**:
  - Existing installs that hardcoded `--set podSecurityContext.runAsUser=65534`
    need to flip to `65532` on their next `helm upgrade`.  Mitigated by the
    release note + the fact that distroless `nonroot` has been the actual
    in-container UID since ADR-0878.
  - Operators who run a permissive CNI and turn on
    `networkPolicy.enabled=true` need to double-check the
    `allow.operatorToApiserver.ports` matrix matches their apiserver
    (default `443` + `6443` covers managed clouds + kubeadm).
  - The default `0.0.0.0/0` egress for the object-store rule is permissive
    on purpose (clusters with public S3/GCS need it); the docs call out the
    follow-up of tightening to a VPC CIDR.

- **Neutral / follow-ups**:
  - Once the chart ships a `vmafx-controller` Service of its own (vs.
    re-using the scoring server's Service), retarget the
    `controller-to-node` allow-rule from "any pod in the release
    namespace" to "pods matching component=controller".
  - Track upstream PSA evolution — Kubernetes 1.31 added a new
    `appArmorProfile` field that we may want to surface once we have a
    bake-in AppArmor profile per backend image.

## References

- ADR-0699 (VMAFX Helm chart) — chart layout and workload-type matrix.
- ADR-0709 (VMAFX Phase 4b distributed platform) — controller/node split that drives the allow-rules.
- ADR-0719 (vmafx-node rclone integration) — motivates the node-egress object-store rule.
- ADR-0878 (Trivy container scan baseline / PR #367) — pins the distroless `nonroot` UID 65532 across all production images.
- Kubernetes Pod Security Admission "restricted" profile: <https://kubernetes.io/docs/concepts/security/pod-security-admission/>
- NetworkPolicy reference: <https://kubernetes.io/docs/concepts/services-networking/network-policies/>
- Distroless `nonroot` UID convention: <https://github.com/GoogleContainerTools/distroless>
- Source: `req` — task instruction "implement modernization #7: add NetworkPolicy + PodSecurityStandards templates to helm chart" (2026-05-31 session).

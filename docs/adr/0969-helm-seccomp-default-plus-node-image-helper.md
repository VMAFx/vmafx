<!-- markdownlint-disable MD013 MD060 -->
# ADR-0969: Helm chart — add seccompProfile default and fix node-deployment image helper (Round 26 audit B.1 + B.3)

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: `security`, `helm`, `kubernetes`

## Context

A Round 26 audit of the VMAFX Helm chart (`deploy/helm/vmafx/`) identified two bugs:

**B.1 — missing `seccompProfile`**: The `podSecurityContext` block in `values.yaml`
only set `runAsNonRoot`, `runAsUser`, `runAsGroup`, and `fsGroup`. The Kubernetes Pod
Security Standards "restricted" admission profile (see PR #439, ADR-0930) requires that
pods set `seccompProfile.type` to `RuntimeDefault` (or `Localhost`). Without it, any
namespace labelled `pod-security.kubernetes.io/enforce=restricted` would reject all
VMAFX pods on admission. Since every workload template (`deployment.yaml`, `job.yaml`,
`statefulset.yaml`, `node-deployment.yaml`) imports `.Values.podSecurityContext`
verbatim, the fix belongs in `values.yaml`.

**B.3 — broken node-deployment image reference**: `templates/node-deployment.yaml`
rendered the node container image as:

```text
{{ .Values.node.image.repository }}:{{ .Values.node.image.tag | default .Chart.AppVersion }}
```

With the default `node.image.repository: ""`, the rendered image was `:3.0.0`
(empty repository + colon + tag) — an invalid image reference that causes Kubernetes
to emit `ImagePullBackOff: invalid reference format`. The named helper
`vmafx.nodeImage` (defined at `templates/_helpers.tpl:145-149`) already handles the
empty-repository case by defaulting to `<image.repository>-node`. The inline
expression bypassed this helper.

PR #439 (`chore/helm-networkpolicy-pss`, ADR-0930) addresses related PSS concerns but
does not modify `node-deployment.yaml`, so B.3 is not a duplicate of that PR. B.1 does
overlap with PR #439's `values.yaml` change; this ADR ships the minimal fix first so
that deployments are unblocked regardless of when PR #439 merges.

## Decision

We will:

1. Add `seccompProfile: { type: RuntimeDefault }` to `podSecurityContext` in
   `values.yaml`, making every VMAFX workload PSA "restricted"-compliant at the
   pod-security level by default.
2. Replace the inline image expression in `templates/node-deployment.yaml` with
   `{{ include "vmafx.nodeImage" . }}`, delegating to the existing helper that
   correctly falls back to `<image.repository>-node` when `node.image.repository`
   is empty.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| B.1: Per-template seccompProfile annotation instead of values.yaml default | Granular; could differ per workload | Requires touching every template; easy to miss future templates; inconsistent with how podSecurityContext is already shared | Centralising in values.yaml is the established pattern |
| B.1: Defer entirely to PR #439 | Only one PR to review | PR #439 is DRAFT with CONFLICTING merge state; installations fail in the interim | Unblocking deployments immediately outweighs the slight overlap |
| B.3: Set a non-empty `node.image.repository` default in values.yaml | Also fixes the empty-ref bug | Hardcodes an opinionated image path; operator overrides still needed; does not fix the root cause (bypassing the helper) | Fix the template (the source of the bug) rather than working around it in values |
| B.3: Keep inline expression, add `if` guard | Minimal template diff | Duplicates the fallback logic from `vmafx.nodeImage`; two code paths to maintain | DRY: the helper exists precisely for this case |

## Consequences

- **Positive**: All VMAFX pods pass PSA "restricted" admission on namespace enforce.
  Node workers always render a valid image reference when installed with default values.
  PR #439 can rebase cleanly on top (seccompProfile already present; no conflict
  expected on the node-deployment.yaml change since PR #439 does not touch that file).
- **Negative**: Operators who were previously suppressing PSA "restricted" warnings
  by patching rendered manifests must now explicitly opt out via a custom
  `podSecurityContext` values override. This is the intended direction per ADR-0930.
- **Neutral / follow-ups**: PR #439 (ADR-0930) will change `runAsUser`/`runAsGroup`
  from `65534` (nobody) to `65532` (distroless nonroot, per ADR-0878) and add
  container-scope `seccompProfile` + `runAsNonRoot`. Those changes are independent of
  this ADR and will land with PR #439.

## References

- Round 26 audit bug report: B.1 and B.3 (user direction, 2026-05-31).
- [ADR-0930](0930-helm-networkpolicy-pss.md) — NetworkPolicy + PSS "restricted" baseline (PR #439); overlaps on seccompProfile but does not fix B.3.
- [ADR-0699](0699-vmafx-helm-chart-k8s.md) — original Helm chart ADR.
- Kubernetes Pod Security Standards "restricted": <https://kubernetes.io/docs/concepts/security/pod-security-admission/#restricted>.
- `templates/_helpers.tpl` — `vmafx.nodeImage` definition at lines 145-149.

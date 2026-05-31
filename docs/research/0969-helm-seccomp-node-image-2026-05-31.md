<!-- markdownlint-disable MD013 MD060 -->
# Research Digest — Helm Chart PSA B.1 + B.3 Fixes (ADR-0969)

**Date**: 2026-05-31
**PR**: fix/helm-seccomp-and-node-image
**ADR**: [ADR-0969](../adr/0969-helm-seccomp-default-plus-node-image-helper.md)

## Summary

Round 26 audit of `deploy/helm/vmafx/` surfaced two distinct bugs. This digest
documents the investigation and resolution.

## B.1 — Missing seccompProfile

### Finding

`values.yaml` `podSecurityContext` was:

```yaml
podSecurityContext:
  runAsNonRoot: true
  runAsUser: 65534
  runAsGroup: 65534
  fsGroup: 65534
```

The Kubernetes PSA "restricted" profile requires `seccompProfile.type` be set
(either `RuntimeDefault` or `Localhost`). Without it, any namespace labelled
`pod-security.kubernetes.io/enforce=restricted` rejects all pods with:

```text
pods "vmafx-..." is forbidden: violates PodSecurity "restricted:latest":
seccompProfile (pod or containers "vmafx" must set securityContext.seccompProfile
to (...))
```

### Deduplication with PR #439

PR #439 (`chore/helm-networkpolicy-pss`, ADR-0930) is DRAFT and CONFLICTING.
Inspection of its diff confirms it adds `seccompProfile: { type: RuntimeDefault }`
to `values.yaml` plus changes UID/GID to 65532 (distroless nonroot, ADR-0878).
Because PR #439 is DRAFT/CONFLICTING and installations fail without the fix,
we ship the minimal `seccompProfile` addition here. PR #439 can rebase cleanly
on top; its additional changes (UID flip, container-scope seccompProfile,
NetworkPolicy) are independent and non-conflicting.

### Fix

Add to `values.yaml` `podSecurityContext`:

```yaml
  seccompProfile:
    type: RuntimeDefault
```

### Verification

```text
helm template test deploy/helm/vmafx/ | grep -A2 seccompProfile
```

Expected: `seccompProfile: { type: RuntimeDefault }` in every workload pod spec.

## B.3 — Broken node-deployment image reference

### Finding

`templates/node-deployment.yaml` line 65:

```yaml
image: {{ .Values.node.image.repository }}:{{ .Values.node.image.tag | default .Chart.AppVersion }}
```

With default `node.image.repository: ""` (empty string), Helm renders:

```text
image: :3.0.0
```

Kubernetes rejects this as an invalid image reference:

```text
ImagePullBackOff: invalid reference format
```

### Root cause

The named helper `vmafx.nodeImage` (defined at `templates/_helpers.tpl:145-149`)
already exists to handle exactly this case:

```text
{{- define "vmafx.nodeImage" -}}
{{- $repo := .Values.node.image.repository | default (printf "%s-node" .Values.image.repository) -}}
{{- $tag  := .Values.node.image.tag        | default .Chart.AppVersion -}}
{{- printf "%s:%s" $repo $tag -}}
{{- end }}
```

The inline expression in `node-deployment.yaml` bypassed this helper entirely.

### Fix

Replace the inline expression with:

```yaml
image: {{ include "vmafx.nodeImage" . }}
```

### Verification

```text
# Default values (empty node.image.repository) — must render non-empty valid ref
helm template test deploy/helm/vmafx/ --set node.enabled=true | grep -E '^\s+image:' | grep -v imagePullPolicy
# Expected: ghcr.io/vmafx/vmafx-server-node:3.0.0

# Explicit override — must use the override
helm template test deploy/helm/vmafx/ --set node.enabled=true --set node.image.repository=ghcr.io/vmafx/vmafx-node | grep -E '^\s+image:' | grep -v imagePullPolicy
# Expected: ghcr.io/vmafx/vmafx-node:3.0.0
```

### Actual output (pre-PR baseline)

```text
image: :3.0.0
```

### Actual output (post-fix)

```text
image: ghcr.io/vmafx/vmafx-server-node:3.0.0
```

## Files changed

| File | Change |
|---|---|
| `deploy/helm/vmafx/values.yaml` | Add `seccompProfile: { type: RuntimeDefault }` to podSecurityContext |
| `deploy/helm/vmafx/templates/node-deployment.yaml` | Replace inline image expr with `{{ include "vmafx.nodeImage" . }}` |
| `deploy/helm/vmafx/AGENTS.md` | Add invariant note: every new podSecurityContext consumer must include seccompProfile |
| `docs/adr/0969-helm-seccomp-default-plus-node-image-helper.md` | ADR |
| `docs/research/0969-helm-seccomp-node-image-2026-05-31.md` | This digest |
| `changelog.d/fixed/0969-helm-seccomp-and-node-image.md` | Changelog fragment |
| `docs/rebase-notes.md` | No rebase impact entry |

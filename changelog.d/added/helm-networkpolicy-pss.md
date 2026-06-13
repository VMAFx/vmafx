- **Helm chart NetworkPolicy + Pod Security Standards baseline (ADR-0930)** —
  `deploy/helm/vmafx/` now renders pods that pass the Kubernetes
  `pod-security.kubernetes.io/enforce=restricted` admission profile out
  of the box: `runAsNonRoot`, distroless `nonroot` UID/GID `65532`
  (aligned with ADR-0878), `readOnlyRootFilesystem`, dropped capabilities,
  `seccompProfile.type=RuntimeDefault`, and `allowPrivilegeEscalation=false`
  at both pod and container scope.  A new opt-in
  `templates/networkpolicy.yaml` (gated by `--set networkPolicy.enabled=true`)
  emits a default-deny ingress + egress baseline plus narrow allow-rules
  for in-namespace HTTP ingress, controller -> node gRPC, node -> object
  store HTTPS (CIDR + `except` matrix), operator -> apiserver, and DNS
  egress to CoreDNS.  `operator-deployment.yaml` and
  `tests/test-connection.yaml` now inherit `podSecurityContext` /
  `securityContext` from `.Values` so chart-wide hardening changes can
  no longer drift across templates.  `NOTES.txt` and
  `docs/development/k8s-deployment.md` document the namespace-labelling
  command and the full NetworkPolicy matrix.

  **Migration**: installs that hard-coded
  `--set podSecurityContext.runAsUser=65534` should drop the override or
  flip it to `65532` to keep file ownership consistent with the
  distroless `nonroot` baked into every production image.

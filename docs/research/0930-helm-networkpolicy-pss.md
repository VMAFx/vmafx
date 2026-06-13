<!-- markdownlint-disable MD013 MD041 MD060 -->
# Research digest — ADR-0930: Helm NetworkPolicy + Pod Security Standards baseline

- **Date**: 2026-05-31
- **Author**: Claude (Anthropic) for lusoris
- **Companion ADR**: [ADR-0930](../adr/0930-helm-networkpolicy-pss.md)
- **Related ADRs**: ADR-0699 (Helm chart), ADR-0709 (Phase 4b platform),
  ADR-0719 (rclone integration), ADR-0878 (Trivy DS-0002 baseline).

## Problem

`deploy/helm/vmafx/` had a partial pod-security baseline (no
`seccompProfile`, no container-level `runAsNonRoot`, drifted UID 65534 vs.
the image's distroless `nonroot` 65532 from ADR-0878) and shipped no
NetworkPolicies.  Operators routinely label production namespaces with
`pod-security.kubernetes.io/enforce=restricted`; the chart needs to render
pods that pass that admission gate without per-install overrides.

## Inventory — what shipped before

| Surface                                  | Before                                                                                                                |
|------------------------------------------|-----------------------------------------------------------------------------------------------------------------------|
| `values.yaml` `podSecurityContext`       | `runAsNonRoot=true`, UID/GID `65534`, no `seccompProfile`.                                                            |
| `values.yaml` `securityContext`          | `allowPrivilegeEscalation=false`, `readOnlyRootFilesystem=true`, `capabilities.drop=[ALL]`. No `runAsNonRoot`, no `runAsUser`, no `seccompProfile`. |
| `templates/deployment.yaml`              | Inherits `.Values.podSecurityContext` + `.Values.securityContext`. OK once the values are fixed.                      |
| `templates/statefulset.yaml`             | Inherits `.Values`. OK once the values are fixed.                                                                     |
| `templates/job.yaml`                     | Inherits `.Values`. OK once the values are fixed.                                                                     |
| `templates/node-deployment.yaml`         | Inherits `.Values`. OK once the values are fixed.                                                                     |
| `templates/operator-deployment.yaml`     | **Hard-coded** `runAsUser=65534`, no seccomp, container `securityContext` hand-rolled (no `runAsNonRoot`).             |
| `templates/tests/test-connection.yaml`   | **Hard-coded** `runAsUser=65534`, no seccomp, container `securityContext` hand-rolled.                                |
| `templates/networkpolicy.yaml`           | Missing.                                                                                                              |
| `docs/development/k8s-deployment.md`     | One-paragraph "Security context" section quoting the old UID.                                                          |
| `NOTES.txt`                              | No PSS namespace label hint, no NetworkPolicy verification snippet.                                                   |

## PSA "restricted" requirements vs. the chart

[K8s reference](https://kubernetes.io/docs/concepts/security/pod-security-admission/#restricted)

| `restricted` check                              | Before                              | After                                       |
|-------------------------------------------------|-------------------------------------|---------------------------------------------|
| `spec.securityContext.runAsNonRoot=true`        | yes                                 | yes (UID flipped 65534 -> 65532)            |
| Container `securityContext.runAsNonRoot=true`   | **missing**                         | yes                                         |
| `seccompProfile.type` in {`RuntimeDefault`, `Localhost`} | **missing**                  | `RuntimeDefault` (both pod + container)     |
| `allowPrivilegeEscalation=false`                | yes                                 | yes                                         |
| `capabilities.drop` >= `[ALL]`                   | yes                                 | yes                                         |
| No `CAP_SYS_ADMIN`, `CAP_NET_ADMIN`, ...        | yes (everything dropped)            | yes                                         |
| `readOnlyRootFilesystem=true` (rec.)             | yes                                 | yes                                         |
| No `hostPath`, no `hostNetwork`, ...            | yes (none used)                     | yes                                         |

## NetworkPolicy matrix shipped

`templates/networkpolicy.yaml` (gated by `networkPolicy.enabled=false`) emits:

| Policy                              | Pod scope                         | Direction | Peer                                     | Ports                | Notes                                                            |
|-------------------------------------|-----------------------------------|-----------|------------------------------------------|----------------------|------------------------------------------------------------------|
| `default-deny`                      | release selector                  | both      | none                                     | none                 | Baseline drop-all; per-component variants when `operator.enabled` / `node.enabled`. |
| `allow-http-ingress`                | release selector                  | ingress   | any pod in namespace                     | `service.targetPort` | Only rendered for `Deployment` / `StatefulSet` workloads.        |
| `allow-controller-to-node`          | `component=node`                  | ingress   | `selectorLabels` (controller pods)       | `50051` (configurable) | Only when `node.enabled=true`.                                  |
| `allow-node-egress-object-store`    | `component=node`                  | egress    | `cidrs` minus `except` (default 0.0.0.0/0 minus RFC1918) | configurable (default 443) | Tightening recommended in production.                            |
| `allow-operator-to-apiserver`       | `component=operator`              | egress    | `0.0.0.0/0` (apiserver IP not selectable) | `443`, `6443`        | Only when `operator.enabled=true`.                              |
| `allow-dns-egress`                  | release selector                  | egress    | `kube-system` / CoreDNS                  | `53/udp`, `53/tcp`   | Required for the other allow-rules to function.                  |

Each allow-rule has its own `enabled` switch under
`networkPolicy.allow.<name>.enabled` so operators can suppress flows their
own CNI policies cover.

## Validation evidence

```bash
$ helm lint deploy/helm/vmafx --strict
==> Linting deploy/helm/vmafx
[INFO] Chart.yaml: icon is recommended
1 chart(s) linted, 0 chart(s) failed

$ helm template deploy/helm/vmafx \
    --set networkPolicy.enabled=true \
    --set operator.enabled=true \
    --set node.enabled=true \
    --set node.image.repository=ghcr.io/vmafx/vmafx-node \
  | kubectl create --dry-run=client --validate=false -f -
networkpolicy.networking.k8s.io/release-name-vmafx-default-deny created (dry run)
networkpolicy.networking.k8s.io/release-name-vmafx-operator-default-deny created (dry run)
networkpolicy.networking.k8s.io/release-name-vmafx-node-default-deny created (dry run)
networkpolicy.networking.k8s.io/release-name-vmafx-allow-http-ingress created (dry run)
networkpolicy.networking.k8s.io/release-name-vmafx-allow-controller-to-node created (dry run)
networkpolicy.networking.k8s.io/release-name-vmafx-allow-node-egress-object-store created (dry run)
networkpolicy.networking.k8s.io/release-name-vmafx-allow-operator-to-apiserver created (dry run)
networkpolicy.networking.k8s.io/release-name-vmafx-allow-dns-egress created (dry run)
...
```

`helm template` with the defaults (`networkPolicy.enabled=false`) emits
zero NetworkPolicy resources — verified via
`helm template deploy/helm/vmafx | grep -c NetworkPolicy` returning `0`.

## Decision summary

See [ADR-0930](../adr/0930-helm-networkpolicy-pss.md) for the full decision
matrix.  Headline trade-offs:

- **Default-off NP** to keep the chart usable on clusters without a
  NetworkPolicy-aware CNI; documented opt-in.
- **UID 65532** to match the distroless `nonroot` baked into every
  production image (ADR-0878), eliminating mixed-ownership bugs on
  writable PVCs / `emptyDir` caches.
- **`RuntimeDefault` seccomp** as the PSA `restricted` minimum; custom
  `Localhost` profiles are an opt-in future iteration when we have
  per-backend AppArmor / seccomp profiles to ship.
- **Inherit security blocks from `.Values`** in every template to
  eliminate the drift between `operator-deployment.yaml`,
  `test-connection.yaml`, and the rest.

## Follow-ups

- When a dedicated `vmafx-controller` Service lands (vs. re-using the
  scoring Service), retarget the `controller-to-node` allow-rule to
  `component=controller` instead of "any pod in namespace".
- Track Kubernetes `appArmorProfile` (1.31+) — surface once we have
  per-backend AppArmor profiles.
- Tighten `nodeEgressObjectStore.cidrs` in the production values overlay
  once we publish a VPC topology guide.

## References

- ADR-0930 (this digest's companion)
- ADR-0699, ADR-0709, ADR-0719, ADR-0878 (cited above)
- <https://kubernetes.io/docs/concepts/security/pod-security-admission/>
- <https://kubernetes.io/docs/concepts/services-networking/network-policies/>
- <https://github.com/GoogleContainerTools/distroless>
- Source: `req` — task instruction "implement modernization #7: add NetworkPolicy + PodSecurityStandards templates to helm chart" (2026-05-31 session).

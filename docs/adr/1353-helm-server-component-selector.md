<!-- markdownlint-disable MD013 MD060 -->
# ADR-1353: Give the Helm server workload its own component selector

- **Status**: Accepted
- **Date**: 2026-09-28
- **Deciders**: lusoris
- **Tags**: helm, kubernetes, release

## Context

The chart's server Deployment (`deploy/helm/vmafx/templates/deployment.yaml`) selected only the release labels, `app.kubernetes.io/name` and `app.kubernetes.io/instance`. The operator and node Deployments select the same two labels plus their own `app.kubernetes.io/component`. The server selector therefore matched the operator and node Pods too, and the `helm test` Pod. Kubernetes documents overlapping controller selectors as unsupported. In the Kubernetes E2E run, `kubectl logs deployment/vmafx` printed the operator's log (`T-HELM-SERVER-DEPLOYMENT-SELECTOR-OVERLAP-2026-09-27`). The StatefulSet variant of the server workload (`statefulset.yaml`) had the same selector.

Scoring traffic was never affected. #1181 added `app.kubernetes.io/component: server` to the server Pod templates and to the Service, headless Service, ServiceMonitor, PodDisruptionBudget and HTTP NetworkPolicy selectors. It deliberately left the workload selectors alone, because `spec.selector` of a Deployment or StatefulSet is immutable: a `helm upgrade` that changes it fails with `spec.selector: Invalid value: ...: field is immutable`.

v1.0.0-rc.1 shipped with the overlapping selectors. Fixing them means that `helm upgrade` from an rc.1 release fails once, whatever else changes.

## Decision

The server Deployment and StatefulSet select `app.kubernetes.io/component: server` in addition to the release labels, so each chart component selects only its own Pods. The change ships in 1.0.0-rc.2 without a chart major version. The upgrade path for an rc.1 install is documented in the [Kubernetes deployment guide](../development/k8s-deployment.md#upgrading-from-100-rc1) and in the changelog: delete the server workload with `--cascade=orphan` and run `helm upgrade` (the new workload adopts the running Pods), or uninstall and install again. The chart `version:` stays `0.1.0`: it is packaging-only and deliberately not coordinated with releases ([ADR-1127](1127-single-semver-release-stream.md)). `scripts/ci/check-helm-selector-isolation.py` checks `helm template` output in the Helm Chart workflow and fails when any workload selector matches another component's Pods.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Change the selectors in rc.2 with a documented delete-and-upgrade (chosen) | Fixes the overlap before 1.0.0, while only release-candidate testers have installs; the orphan-delete path keeps the Pods serving | Every rc.1 install needs one manual step before its first upgrade | Maintainer decision, 2026-09-27 |
| Change the selectors with a chart major version bump | Signals the break through the chart version | Nothing publishes the chart version, so the bump would warn nobody; it would still need the same manual step | Rejected by the maintainer |
| Keep the overlapping selectors (status quo since #1181) | No upgrade step | Unsupported controller overlap stays in 1.0.0 and becomes harder to remove after it | Leaves a known defect in the final release |
| Fix only the Deployment, not the StatefulSet | Smaller change | The StatefulSet has the same overlap and would need a second breaking upgrade later | One migration costs less than two |
| A chart `lookup` guard that fails the upgrade with instructions | Clearer message than Kubernetes' immutable-field error | Adds template logic that `helm template` and GitOps renderers cannot exercise, for a one-time migration | The documented error text and upgrade section cover it |

## Consequences

- **Positive**: `kubectl logs deployment/<release>` and any query by the workload's selector see only server Pods, and no two chart controllers compete for a Pod. The check keeps any new component from reintroducing an overlap.
- **Negative**: `helm upgrade` from a v1.0.0-rc.1 release fails until the server Deployment or StatefulSet is deleted. Argo CD and Flux report the same error.
- **Neutral / follow-ups**: The Kubernetes E2E creates a fresh kind cluster for every run, so it installs the chart and never upgrades it. The upgrade path was exercised by hand on kind v1.37.0 with Helm 4.2.4, from the v1.0.0-rc.1 chart and images: the plain upgrade failed for both workloads; after `kubectl delete --cascade=orphan` the upgrade succeeded, the new Deployment adopted the running ReplicaSet and Pod without a restart and then rolled a template change normally; the StatefulSet adopted its Pod and kept its PVC; delete without `--cascade=orphan` followed by the upgrade also succeeded.

## References

- req: "ship in RC2 with an upgrade note telling rc.1 testers to delete and reinstall (NOT a chart major bump)" (maintainer decision, 2026-09-27, recorded in the `docs/state.md` row).
- `T-HELM-SERVER-DEPLOYMENT-SELECTOR-OVERLAP-2026-09-27`; `T-E2E-K8S-FIXTURE-BELOW-DEFAULT-MODEL-2026-09-27` (where the overlap was found).
- [Research: E2E Kubernetes runtime contract](../research/e2e-k8s-runtime-contract-2026-08-31.md) (#1181, the Pod-template and Service selector fix).
- [ADR-1127](1127-single-semver-release-stream.md) (chart packaging version is independent).
- Kubernetes documentation, Deployments, "Label selector updates" and "Selector": overlapping selectors between controllers are not supported.

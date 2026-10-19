<!-- markdownlint-disable MD013 MD060 -->
# ADR-2647: the operator records events in every namespace through a write-only ClusterRole

- **Status**: Accepted
- **Date**: 2026-10-08
- **Deciders**: maintainer, agent
- **Tags**: helm, rbac, operator, security, k8s, fork-local

## Context

[ADR-1058](1058-helm-chart-security-hardening.md) split the operator's RBAC
into a `ClusterRole` for its custom resources and a `Role` in the release
namespace for pods, events and leader-election leases. The custom-resource
grant is cluster-wide on purpose: the chart has no list of watched namespaces,
the operator's controller-runtime cache is not limited to one, and it
reconciles `VmafxJob`, `VmafxNode` and `VmafxModelTraining` objects wherever
they are created.

The `VmafxModelTraining` reconciler records an event when a checkpoint is
written. client-go's recorder (`tools/record`, `makeEvent`) creates the event
in the namespace of the object it is about, so for a resource outside the
release namespace the API server refused the event: the `Role` reached only
the release namespace. The reconciler logs the refusal and goes on, so nothing
failed visibly; the event was lost. The maintainer asked for the grant to
follow the chart's namespace model (2026-10-08).

## Decision

The chart gives the operator's service account a second `ClusterRole`,
`<fullname>-operator-events`, bound cluster-wide, with `create` and `patch` on
core `events` and nothing else; the release-namespace `Role` keeps pods and
leases only. The event grant reaches exactly as far as the custom-resource
grant, because an event is written where the resource it describes lives, and
it stays write-only: the operator cannot read, list, watch, update or delete
events anywhere. The custom-resource `ClusterRole` stays limited to the
custom resources, as ADR-1058 has it.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| A write-only events `ClusterRole` bound cluster-wide (chosen) | Covers every namespace the operator reconciles; no new values; read access to events stays denied | Cluster-wide write of event records | — |
| Add the events rule to the custom-resource `ClusterRole` | One object less | Mixes a core resource into the role ADR-1058 keeps to the custom resources; the events grant is no longer reviewable on its own | Two separate grants are clearer at the same reach |
| A `Role` and `RoleBinding` per namespace from a new `operator.namespaces` list | Event writes limited to listed namespaces | The chart renders into the release namespace only and has no namespace list; the operator still watches every namespace, so objects outside the list would lose their events silently, as before | Needs a namespace model the chart and the operator do not have; would only move the bug |
| Keep the release-namespace `Role` and document the limit | No change | Events of resources in other namespaces stay lost | Leaves the defect |
| Limit the operator to the release namespace | Every grant namespaced | Changes the operator's documented scope and breaks clusters with resources in several namespaces | Out of proportion for an event grant |

## Consequences

- **Positive**: checkpoint events of `VmafxModelTraining` resources reach their
  namespace; leader-election events keep working in the release namespace.
- **Negative**: the operator can create and patch event records in any
  namespace. It could already reconcile and update its custom resources in
  any namespace, and it gains no read access.
- **Neutral / follow-ups**: `scripts/ci/tests/test_helm_service_accounts.py`
  resolves bindings per namespace and fails when events are granted only in
  the release namespace, when the grant gains a read verb, or when pods or
  leases leave the release namespace. ADR-1058's status line points here for
  the events rule. If the operator ever gets a namespace list, this grant
  moves to per-namespace `Role`s with it.

## References

- [ADR-1058](1058-helm-chart-security-hardening.md) — the RBAC split this
  changes for events.
- [ADR-1592](1592-helm-split-service-accounts.md) — service accounts and the
  tenant reader.
- `req` (maintainer, via the coordinator, 2026-10-08): "Operator events RBAC: fix now as its own small PR from master — grant event creation where the model-training controller records them, consistent with the chart's tenancy model (ClusterRole vs per-namespace Roles: pick per the chart's existing namespace handling and say why), a test that fails on the current chart, and a docs/state.md row closed by the PR."

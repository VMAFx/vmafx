## Operator events cluster-wide (2026-10-08)

`fix/operator-events-rbac`, [ADR-2647](adr/2647-operator-events-cluster-wide.md).
`deploy/helm/vmafx/templates/operator-rbac.yaml` renders a third operator
role, the `ClusterRole` `<fullname>-operator-events` with its binding
(`create`, `patch` on core `events` only), and the release-namespace `Role`
no longer lists events. A rebase that touches the template keeps events out
of the namespace `Role` and out of the custom-resource `ClusterRole`;
`scripts/ci/tests/test_helm_service_accounts.py` fails otherwise. no upstream
file.

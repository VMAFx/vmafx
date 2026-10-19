## Helm controller on PostgreSQL and its failover E2E case (2026-10-07)

`rc4/api-wp17-chart`, [ADR-2350](adr/2350-cloud-native-platform.md). The
controller Deployment takes its replica count, update strategy, volume and
store environment from the `_helpers.tpl` helpers `vmafx.controllerStoreBackend`,
`vmafx.controllerStoreEnv`, `vmafx.controllerDatabaseDSN` and
`vmafx.controllerTopologySpread`; a sync that touches `controller.yaml`,
`pdb.yaml` or `networkpolicy.yaml` keeps them, and keeps the SQLite store at
one replica. `controller.store.*` holds one key per setting so the generator
of the values and schema (ADR-2350 work package 4) reproduces them unchanged.
The E2E case `02-controller-ha` moves together with
`.github/workflows/e2e-k8s.yml`, `test/e2e/kind-cluster.sh` and
`scripts/ci/test_e2e_runtime_contract.py`. no upstream file.

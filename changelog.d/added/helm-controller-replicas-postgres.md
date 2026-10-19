- **The Helm chart runs vmafx-controller as several replicas on PostgreSQL
  ([ADR-2350](docs/adr/2350-cloud-native-platform.md)).**
  `controller.store.backend: postgres` with `controller.replicas` renders a
  rolling-update controller Deployment without a volume, a CloudNativePG
  `Cluster` (`postgresql.mode: cnpg`, the operator is a prerequisite) or the
  connection URI of an external database from a Secret, and a migration Job
  that runs `vmafx-controller migrate` once per controller image. Several
  replicas bring a PodDisruptionBudget and a spread across nodes, and
  `networkPolicy.enabled` opens the database to the controller. The SQLite
  store stays the default and one replica; the chart refuses more. A new E2E
  case kills a controller replica and the node of a running job mid-job on
  kind and checks that every job is completed exactly once
  ([integration tests](docs/k8s/integration-tests.md#controller-failover-case),
  [job store and replicas](docs/development/k8s-deployment.md#controller-store)).

- **vmafx-controller can keep its jobs in PostgreSQL and run as several
  replicas ([ADR-2350](docs/adr/2350-cloud-native-platform.md)).**
  `VMAFX_STORE_BACKEND=postgres` with `VMAFX_DB_DSN` stores jobs, attempts and
  node sessions in PostgreSQL: any replica serves any node, a node keeps
  working through a controller restart without registering again, a pulled
  job is leased and returns to the queue when its node stops renewing it, and
  a late report from a node that lost its lease is refused, so a job keeps one
  result. `vmafx-controller migrate` applies the schema and
  `vmafx-controller import-sqlite --from <file>` copies the jobs of the SQLite
  queue. `/readyz` answers not ready while the database is unreachable or its
  schema is older than the controller needs. The SQLite queue stays the
  default.

- **The distributed platform has an architecture for running without local state
  ([ADR-2350](docs/adr/2350-cloud-native-platform.md)).** Job state and node
  sessions move from the controller's embedded SQLite queue to PostgreSQL, so
  several controller replicas can serve any node; nodes keep the gRPC protocol
  and claim work through leases; River runs retries and follow-up steps; node
  pools scale on queue depth with KEDA; results become signed OCI artifacts; job
  events go out as CloudEvents; and the protobuf, CRDs, OpenAPI and Helm values
  schema are generated from a platform definition. A standalone profile keeps
  SQLite. This change only records the decision: the chart, the controller and
  the documentation pages say that the single-replica SQLite queue is
  transitional. Nothing in a running installation changes.

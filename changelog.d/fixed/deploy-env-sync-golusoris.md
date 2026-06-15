- **k8s deploy manifests synced to the golusoris env contract (ADR-1119):** the
  fx-migrated binaries adopt golusoris-native defaults (gRPC `:9090`) and renamed
  env vars, but the Helm chart / Dockerfiles still referenced the pre-fx contract,
  which would have broken deploys. The node deployment now pins `VMAFX_GRPC_LISTEN`
  to its `grpcPort` (so the Service + tcpSocket probes reach the listener),
  `VMAFX_OPERATOR_LOG_LEVEL` → `VMAFX_LOG_LEVEL`, and the operator Dockerfile/value
  docs use the new `VMAFX_OPERATOR_HEALTH_PROBE_ADDR` / `_LEADER_ELECTION` names.

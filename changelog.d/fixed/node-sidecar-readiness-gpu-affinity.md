- **`vmafx-node` no longer dials an undeployed online-training sidecar, gates
  `/readyz` on the sidecar when enabled, and supports node-scoped pod affinity
  in the Helm chart
  ([ADR-3558](docs/adr/3558-node-sidecar-readiness-gpu-affinity.md)).** The
  sidecar feedback drainer is now opt-in via `VMAFX_SIDECAR_ENABLED`
  (`sidecar.enabled`, chart `node.sidecar.enabled`, default `false`), stopping
  recurring `WARN sidecar not reachable` logs on standard deployments while
  rejecting invalid boolean values at startup. When enabled, `/readyz` reports
  `503` unless the drainer holds a live Unix socket connection (with reconnect
  backoff capped at 15 s so readiness recovers within 30 s). The Helm chart's
  node `readinessProbe` now queries HTTP `/readyz` on `metrics` (backed by
  `scorer`, loopback `grpc`, and conditional `sidecar` checks, amending
  ADR-1094), and `node.affinity` overrides global `affinity` so heterogeneous
  GPU pools can configure soft scheduling preferences without a hard pin. For
  Intel Arc pools, use `gpu.intel.com/product` with `Arc_Pro_B60` preferred
  (weight 100) and `Arc_B580` as fallback (weight 50).

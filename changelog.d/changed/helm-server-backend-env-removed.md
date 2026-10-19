- **The chart no longer sets `VMAFX_BACKEND` on the scoring server.** The
  server's Deployment, StatefulSet and Job carried it, but `vmafx-server`
  never read it: it takes its backend from each request's `backend` score
  option. The install notes no longer print a `BACKEND` line, and the server
  container has an `env` list only when `env` holds values. Nodes keep
  `VMAFX_BACKEND` from `gpu.vendor`. The unused named templates
  `vmafx.podSpec`, `vmafx.containerSpec`, `vmafx.volumes` and
  `vmafx.sidecarContainer` are removed; no chart template included them.
  `vmafx-mcp` no longer copies `VMAFX_LOG_LEVEL` and `VMAFX_LOG_FORMAT` into
  `LOG_LEVEL` and `LOG_FORMAT`, which nothing read. The upgrade notes are in
  `docs/development/k8s-deployment.md`
  ([ADR-2350](docs/adr/2350-cloud-native-platform.md)).

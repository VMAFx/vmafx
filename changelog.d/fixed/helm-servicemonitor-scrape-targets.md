- **Helm: the server's ServiceMonitor no longer scrapes StatefulSet pods
  twice, and finds the release from another namespace.** With
  `workload: StatefulSet` it also matched the headless Service, so every
  server pod was a second target and sums over the server's series doubled;
  it now skips Services labelled `vmafx.dev/headless`. With
  `monitoring.serviceMonitor.namespace` set to another namespace it selected
  nothing; it now selects the release namespace. The node's HTTP listener
  follows `node.metricsPort` (`VMAFX_HTTP_ADDR`).

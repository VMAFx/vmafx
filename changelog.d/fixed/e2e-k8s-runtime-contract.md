- **Kubernetes E2E runtime contract** — explicitly builds the CPU node and Go
  server targets, transfers every chart image into kind, and replaces four
  unreachable controller scenarios with a default-Helm-workload CPU scoring
  smoke. Exact-local image pulls, fixture validation, real `/v1/score` output,
  the corrected `-m path=…` server-to-CLI model argument, and an always-on
  workflow contract now fail closed when the nightly lane drifts from
  production behavior. A dedicated kubeconfig guard also refuses any context
  other than the named local kind cluster. Server component selectors prevent
  operator metrics from entering the scoring Service, and node images now put
  packaged models at the configured model root. Security Scans concurrency is
  now event-scoped so a weekly schedule cannot cancel CodeQL for a master push.

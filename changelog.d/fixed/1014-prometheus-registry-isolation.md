- **Prometheus registry isolation** (`pkg/observability`): `SetControllerSources` now
  registers the three controller live-gauge metrics (`jobs_pending`, `jobs_running`,
  `nodes_live`) against the isolated `*prometheus.Registry` supplied to `NewMetrics`,
  not the global `DefaultRegisterer`. Fixes gauges being invisible on the `/metrics`
  endpoint and eliminates an `AlreadyRegistered` panic on any second call. A `sync.Once`
  guard makes the method idempotent (ADR-1014).
- **`WaitForShutdown` timer leak** (`pkg/observability`): replaced `time.After(timeout)`
  with `time.NewTimer` + `defer t.Stop()` so the timer is released when the shutdown
  completes rather than lingering for the full 30 s (ADR-1017 fix bundled here).

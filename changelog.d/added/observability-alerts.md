- **Observability: alerts, recording rules and runbooks (RC4, ADR-2349,
  #2430).** `deploy/prometheus/vmafx-rules.yaml` is generated from the metric
  definition: recording rules for the job failure, Score error and slow Score
  ratios (5m, 30m, 1h, 6h) and the hourly score median, and eight alerts:
  `VMAFxComponentDown`, `VMAFxNoLiveNodes`, `VMAFxQueueAging`, the
  multi-window burn-rate alerts `VMAFxJobErrorBudgetBurn`,
  `VMAFxScoreErrorBudgetBurn` and `VMAFxScoreLatencyBudgetBurn` (objectives
  99 %), `VMAFxScoreRegression` and `VMAFxMetricsReadErrors`. Each links a
  runbook page under `docs/observability/runbooks/`, and its promtool unit
  test (`deploy/prometheus/vmafx-rules.test.yaml`) has a firing and a
  non-firing case; `make check-prometheus-rules` runs them with a pinned
  promtool. See [alerts](docs/development/observability.md#alerts-and-recording-rules).

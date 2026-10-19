- **SLO report, usage and cost, and capacity dashboards (RC4, ADR-2349,
  #2430).** `VMAFx SLO report` shows each SLO's compliance, objective, error
  budget left and burn rate over its time range (30 days by default), from
  the objectives the rules record (`vmafx:slo_objective`).
  `VMAFx Usage and cost` counts per tenant the finished jobs, their run time
  and the scores, and prices them at `monitoring.cost.perJobSecond` (the run
  time of every finished job) and `monitoring.cost.perJob` (completed and
  failed jobs; cancelled jobs are not charged), the same for every backend,
  in `monitoring.cost.currency`; an unset price leaves its cost panels empty.
  `VMAFx Capacity` sets the nodes' measured capacity against the demand, with
  the headroom, the demand's growth and linear forecasts of the queue and the
  demand. The Helm chart ships them like the other dashboards; the Compose
  smoke test checks them. See
  [the dashboard tour](docs/observability/dashboards.md#slo-report).

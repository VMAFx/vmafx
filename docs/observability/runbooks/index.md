# Alert runbooks

One page per alert of the generated rule file
(`deploy/prometheus/vmafx-rules.yaml`). Every alert carries a `runbook_url`
annotation that links its page here. Each page says what the alert means,
what it costs, how to find the cause and how to fix it.

| Alert | Severity | Page |
| --- | --- | --- |
| `VMAFxComponentDown` | critical | [Component down](vmafx-component-down.md) |
| `VMAFxNoLiveNodes` | critical | [No live nodes](vmafx-no-live-nodes.md) |
| `VMAFxQueueAging` | warning | [Queue aging](vmafx-queue-aging.md) |
| `VMAFxJobErrorBudgetBurn` | critical (fast), warning (slow) | [Job error budget burn](vmafx-job-error-budget-burn.md) |
| `VMAFxScoreErrorBudgetBurn` | critical (fast), warning (slow) | [Score error budget burn](vmafx-score-error-budget-burn.md) |
| `VMAFxScoreLatencyBudgetBurn` | critical (fast), warning (slow) | [Score latency budget burn](vmafx-score-latency-budget-burn.md) |
| `VMAFxScoreRegression` | warning | [Score regression](vmafx-score-regression.md) |
| `VMAFxMetricsReadErrors` | warning | [Metrics read errors](vmafx-metrics-read-errors.md) |

The burn-rate alerts follow the multi-window, multi-burn-rate pattern. With
the default settings the fast rule (critical) fires when an hour and its last
five minutes spend the error budget 14.4 times faster than the 30-day
objective allows, which uses 2 % of the budget in that hour; the slow rule
(warning) fires at 6 times over six hours and their last thirty minutes, 5 %
of the budget. Both windows have to agree, so an alert clears soon after the
cause does. The default objectives are 99 % for each of jobs, Score request
errors and Score requests within 30 seconds. The objectives, windows, factors
and the other alerts' thresholds are settings: chart values on Kubernetes
([monitoring on Kubernetes](../kubernetes.md#set-the-objectives-and-thresholds)),
the same keys for a rule file rendered with `go run ./tools/obsgen
-render-rules`.

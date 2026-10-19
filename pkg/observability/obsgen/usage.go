// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package obsgen

import (
	"github.com/grafana/grafana-foundation-sdk/go/dashboard"

	m "github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

// UsageUID is the usage and cost dashboard's uid.
const UsageUID = "vmafx-usage"

// tenantJobSeconds is the run time of the tenant's finished jobs over the
// window, in seconds.
func tenantJobSeconds(window string) string {
	return "sum by (tenant) (increase(" + sel(m.ControllerJobDuration.Name+"_sum", tenantMatcher) + "[" + window + "]))"
}

// chargedOutcomes are the outcomes the per-job price applies to (decision
// Q-208): completed and failed jobs; a cancelled job is not charged.
const chargedOutcomes = `outcome=~"completed|failed"`

// tenantJobs is the number of the tenant's jobs that finished over the
// window with the given outcome matchers, by tenant and by.
func tenantJobs(window, by string, outcomes ...string) string {
	return sumBy(by, "increase("+sel(m.ControllerJobDuration.Name+"_count", append([]string{tenantMatcher}, outcomes...)...)+"["+window+"])")
}

// PerJobCostQuery is the per-job cost by tenant over window: charged jobs
// times the recorded per-job price.
func PerJobCostQuery(window string) string {
	return priced(tenantJobs(window, "tenant", chargedOutcomes), PricePerJob)
}

// priced multiplies a per-tenant usage by a recorded price. The price carries
// the currency; an unset price records nothing, and the cost is empty.
func priced(usage, price string) string {
	return usage + " * on () group_left (currency) max by (currency) (" + sel(price) + ")"
}

// usage answers, per tenant over the chosen window: how many jobs finished,
// how much node time they took, how many scores were produced, and, with
// prices configured, what that cost. The cost model is the rule settings'
// monitoring.cost: one price per second of job run time (every outcome) and
// one per completed or failed job, the same for every backend (decisions
// Q-193, Q-208); unset prices leave the cost panels empty.
func usage() *dashboard.DashboardBuilder {
	b := newDashboard(UsageUID, "VMAFx Usage and cost",
		"Per-tenant usage over the dashboard's time range (7 days by default): finished jobs, node run time and scores, and their cost at the prices of the rule settings (monitoring.cost: one price per job-second and one per job, the same for every backend). Without prices the cost panels stay empty. Generated from pkg/observability/metricdef by tools/obsgen.",
		m.ControllerJobsSubmitted, tenantVariable()).Time("now-7d", "now")
	runCost := priced(tenantJobSeconds("$__range"), PriceJobSecond)
	jobCost := PerJobCostQuery("$__range")
	return withRows(b, []row{
		{"Usage over the time range", []panelBuilder{
			statPanel("Finished jobs by tenant and outcome", "Controller jobs that finished in the time range, by outcome. The per-job price applies to completed and failed jobs; cancelled jobs are not charged.", unitCount,
				steps("blue"), instant(tenantJobs("$__range", "tenant, outcome"), "{{tenant}} {{outcome}}")).Span(8),
			statPanel("Job run time by tenant", "Node time the tenant's finished jobs took in the time range: what the per-second price applies to.", unitSeconds,
				steps("blue"), instant(tenantJobSeconds("$__range"), "{{tenant}}")).Span(8),
			statPanel("Scores by tenant", "Pooled scores produced for the tenant in the time range, by jobs and by Score requests on the controller.", unitCount,
				steps("blue"), instant("sum by (tenant) (increase("+sel(m.QualityScore.Name+"_count", tenantMatcher)+"[$__range]))", "{{tenant}}")).Span(8),
		}},
		{"Cost over the time range", []panelBuilder{
			statPanel("Run-time cost by tenant", "Job run time times the price per job-second (monitoring.cost.perJobSecond). Empty while the price is unset.", unitCount,
				steps("blue"), instant(runCost, "{{tenant}} {{currency}}")).Span(8),
			statPanel("Per-job cost by tenant", "Completed and failed jobs times the price per job (monitoring.cost.perJob); cancelled jobs are not charged. Empty while the price is unset.", unitCount,
				steps("blue"), instant(jobCost, "{{tenant}} {{currency}}")).Span(8),
			statPanel("Total cost by tenant", "Run-time cost plus per-job cost; either alone when only one price is set.", unitCount,
				steps("blue"), instant("("+runCost+" + "+jobCost+") or "+runCost+" or "+jobCost, "{{tenant}} {{currency}}")).Span(8),
		}},
		{"Usage over time", []panelBuilder{
			timeseriesPanel("Node time per hour by tenant", "Run time of the tenant's jobs finishing per hour.", unitSeconds,
				query(tenantSum("rate("+sel(m.ControllerJobDuration.Name+"_sum", tenantMatcher)+rateWindow+")")+" * 3600", "{{tenant}}")),
			timeseriesPanel("Jobs per hour by tenant", "The tenant's jobs finishing per hour, every outcome.", unitCount,
				query(tenantSum("rate("+sel(m.ControllerJobDuration.Name+"_count", tenantMatcher)+rateWindow+")")+" * 3600", "{{tenant}}")),
		}},
	})
}

// tenantSum sums expr by tenant.
func tenantSum(expr string) string {
	return sumBy("tenant", expr)
}

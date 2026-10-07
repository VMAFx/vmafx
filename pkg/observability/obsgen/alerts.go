// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package obsgen

import (
	m "github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

// Thresholds of the alerts that are not SLO burn rates.
const (
	// QueueAgeLimit is how old, in seconds, a tenant's oldest pending job
	// may get before VMAFxQueueAging fires.
	QueueAgeLimit = "1800"
	// ScoreRegressionPoints is the drop of an hour's median score below the
	// median of the previous day's hourly medians that VMAFxScoreRegression
	// reports.
	ScoreRegressionPoints = "5"
	// ScoreRegressionMinScores is the number of scores an hour needs before
	// its median is compared.
	ScoreRegressionMinScores = "20"
)

// series names one input series of a promtool case.
func series(name, labels, values string) inputSeries {
	return inputSeries{Series: name + "{" + labels + "}", Values: values}
}

// alerts lists every alert, in the order of the rule file.
func alerts() []alert {
	ratios := sloRatios()
	return []alert{
		componentDown(), noLiveNodes(), queueAging(),
		{name: "VMAFxJobErrorBudgetBurn", runbook: "vmafx-job-error-budget-burn",
			rules: burnRules("Controller job failure", ratios[0]),
			cases: burnCases(m.ControllerJobsFailed.Name, m.ControllerJobsCompleted.Name, `tenant="a"`)},
		{name: "VMAFxScoreErrorBudgetBurn", runbook: "vmafx-score-error-budget-burn",
			rules: burnRules("Score request error", ratios[1]),
			cases: errorBudgetCases(m.ServerScoreErrors.Name, m.ServerScoreRequests.Name)},
		{name: "VMAFxScoreLatencyBudgetBurn", runbook: "vmafx-score-latency-budget-burn",
			rules: burnRules("Slow Score request", ratios[2]),
			cases: latencyBudgetCases()},
		scoreRegression(), metricsReadErrors(),
	}
}

func componentDown() alert {
	up := series("up", `job="vmafx-node",instance="n1:9090"`, "1x10 0x20")
	info := series(m.BuildInfo.Name, `job="vmafx-node",instance="n1:9090",version="1.0.0"`, "1x10")
	other := series("up", `job="other",instance="x:80"`, "0x30")
	return alert{
		name: "VMAFxComponentDown", runbook: "vmafx-component-down",
		rules: []alertRule{{
			expr:        "up == 0 and on (job, instance) max_over_time(" + m.BuildInfo.Name + "[1h])",
			forDur:      "5m",
			severity:    "critical",
			summary:     "VMAFx target {{ $labels.instance }} of {{ $labels.job }} is down.",
			description: "Prometheus has not scraped {{ $labels.instance }} ({{ $labels.job }}) for 5 minutes; it served vmafx_build_info within the last hour.",
		}},
		cases: []alertCase{
			{interval: "1m", series: []inputSeries{up, info}, evalTime: "30m",
				firing: []map[string]string{{"job": "vmafx-node", "instance": "n1:9090", "severity": "critical"}}},
			{interval: "1m", series: []inputSeries{other}, evalTime: "30m"},
		},
	}
}

func noLiveNodes() alert {
	pending := series(m.ControllerJobsPending.Name, `tenant="a"`, "3x20")
	return alert{
		name: "VMAFxNoLiveNodes", runbook: "vmafx-no-live-nodes",
		rules: []alertRule{{
			expr:        "max(" + m.ControllerNodesLive.Name + ") == 0 and on () sum(" + m.ControllerJobsPending.Name + ") > 0",
			forDur:      "10m",
			severity:    "critical",
			summary:     "Jobs are waiting and no vmafx-node is registered with the controller.",
			description: "The controller has pending jobs and no live node for 10 minutes; nothing can run them.",
		}},
		cases: []alertCase{
			{interval: "1m", series: []inputSeries{series(m.ControllerNodesLive.Name, "", "0x20"), pending}, evalTime: "15m",
				firing: []map[string]string{{"severity": "critical"}}},
			{interval: "1m", series: []inputSeries{series(m.ControllerNodesLive.Name, "", "1x20"), pending}, evalTime: "15m"},
		},
	}
}

func queueAging() alert {
	return alert{
		name: "VMAFxQueueAging", runbook: "vmafx-queue-aging",
		rules: []alertRule{{
			expr:        "max by (tenant) (" + m.ControllerQueueOldestAge.Name + ") > " + QueueAgeLimit,
			forDur:      "15m",
			severity:    "warning",
			summary:     "Jobs of tenant {{ $labels.tenant }} wait longer than 30 minutes.",
			description: "The oldest pending job of tenant {{ $labels.tenant }} has waited more than 30 minutes for 15 minutes: no node takes its work.",
		}},
		cases: []alertCase{
			{interval: "1m", series: []inputSeries{series(m.ControllerQueueOldestAge.Name, `tenant="a"`, "0+60x60")}, evalTime: "50m",
				firing: []map[string]string{{"tenant": "a", "severity": "warning"}}},
			{interval: "1m", series: []inputSeries{series(m.ControllerQueueOldestAge.Name, `tenant="a"`, "60x60")}, evalTime: "50m"},
		},
	}
}

// bothBurns are the firing label sets of a fast and a slow burn together.
var bothBurns = []map[string]string{{"severity": "critical"}, {"severity": "warning"}}

// burnCases: half the events bad for an hour burns both budgets; none bad
// burns neither.
func burnCases(bad, good, labels string) []alertCase {
	return []alertCase{
		{interval: "1m", evalTime: "1h", firing: bothBurns,
			series: []inputSeries{series(bad, labels, "0+30x60"), series(good, labels, "0+30x60")}},
		{interval: "1m", evalTime: "1h",
			series: []inputSeries{series(bad, labels, "0x60"), series(good, labels, "0+30x60")}},
	}
}

// errorBudgetCases: errors and requests are counted separately, so half the
// requests failing is errors at half the request rate.
func errorBudgetCases(errors, requests string) []alertCase {
	return []alertCase{
		{interval: "1m", evalTime: "1h", firing: bothBurns,
			series: []inputSeries{series(errors, "", "0+5x60"), series(requests, "", "0+10x60")}},
		{interval: "1m", evalTime: "1h",
			series: []inputSeries{series(errors, "", "0x60"), series(requests, "", "0+10x60")}},
	}
}

func latencyBudgetCases() []alertCase {
	fast := m.ServerScoreDuration.Name + "_bucket"
	all := m.ServerScoreDuration.Name + "_count"
	le := `le="` + ScoreLatencyThreshold + `"`
	return []alertCase{
		{interval: "1m", evalTime: "1h", firing: bothBurns,
			series: []inputSeries{series(fast, le, "0+5x60"), series(all, "", "0+10x60")}},
		{interval: "1m", evalTime: "1h",
			series: []inputSeries{series(fast, le, "0+10x60"), series(all, "", "0+10x60")}},
	}
}

func scoreRegression() alert {
	expr := "vmafx:quality_score:p50_1h < quantile_over_time(0.5, vmafx:quality_score:p50_1h[1d] offset 1h) - " +
		ScoreRegressionPoints + " and vmafx:quality_score:count_1h >= " + ScoreRegressionMinScores
	return alert{
		name: "VMAFxScoreRegression", runbook: "vmafx-score-regression",
		rules: []alertRule{{
			expr: expr, forDur: "1h", severity: "warning",
			summary:     "Median score of {{ $labels.model }} for tenant {{ $labels.tenant }} dropped.",
			description: "For an hour the median pooled score of tenant {{ $labels.tenant }} on model {{ $labels.model }} has been more than 5 points below the median of the previous day's hourly medians.",
		}},
		cases: []alertCase{
			{interval: "10m", evalTime: "26h", series: regressionSeries(true),
				firing: []map[string]string{{"tenant": "a", "model": "model-a", "severity": "warning"}}},
			{interval: "10m", evalTime: "26h", series: regressionSeries(false)},
		},
	}
}

// regressionSeries is a day of scores in (94, 96], then two hours in (75, 80]
// when drop is set (median 95, then 77.5), at 30 scores per 10 minutes.
func regressionSeries(drop bool) []inputSeries {
	labels := func(le string) string { return `tenant="a",model="model-a",profile="none",le="` + le + `"` }
	low := "0x157"
	if drop {
		low = "0x143 0+30x13"
	}
	bucket := m.QualityScore.Name + "_bucket"
	return []inputSeries{
		series(bucket, labels("75"), "0x157"),
		series(bucket, labels("80"), low),
		series(bucket, labels("94"), low),
		series(bucket, labels("96"), "0+30x157"),
		series(bucket, labels("+Inf"), "0+30x157"),
		series(m.QualityScore.Name+"_count", `tenant="a",model="model-a",profile="none"`, "0+30x157"),
	}
}

func metricsReadErrors() alert {
	labels := `job="vmafx-node",instance="n1:9090",source="device_memory"`
	return alert{
		name: "VMAFxMetricsReadErrors", runbook: "vmafx-metrics-read-errors",
		rules: []alertRule{{
			expr:        "sum by (job, instance, source) (rate(" + m.MetricsReadErrors.Name + "[10m])) > 0",
			forDur:      "15m",
			severity:    "warning",
			summary:     "{{ $labels.instance }} cannot read its {{ $labels.source }} metrics.",
			description: "Every scrape of {{ $labels.instance }} ({{ $labels.job }}) for 15 minutes failed to read {{ $labels.source }}; those series are missing from its /metrics page.",
		}},
		cases: []alertCase{
			{interval: "1m", evalTime: "30m", series: []inputSeries{series(m.MetricsReadErrors.Name, labels, "0+1x40")},
				firing: []map[string]string{{"job": "vmafx-node", "instance": "n1:9090", "source": "device_memory", "severity": "warning"}}},
			{interval: "1m", evalTime: "30m", series: []inputSeries{series(m.MetricsReadErrors.Name, labels, "0x40")}},
		},
	}
}

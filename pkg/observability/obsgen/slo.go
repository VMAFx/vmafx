// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package obsgen

import (
	"github.com/grafana/grafana-foundation-sdk/go/dashboard"

	m "github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

// SLOUID is the SLO report dashboard's uid.
const SLOUID = "vmafx-slo"

// sloReport is one SLO of the report: its label on the recorded series and
// what it counts.
type sloReport struct {
	label, title, good string
}

// sloReports are the three SLOs, in the order of the settings.
var sloReports = []sloReport{
	{"job_success", "Jobs", "controller jobs that complete rather than fail"},
	{"score_success", "Score errors", "Score requests that return no error"},
	{"score_latency", "Score latency", "Score requests that finish within the latency bound"},
}

// sloSeries selects one SLO of a recorded settings series.
func sloSeries(series, slo string) string {
	return sel(series, `slo="`+slo+`"`)
}

// badRatio is the share of bad events of an SLO over window: the recorded
// 5-minute rates added up over it, so every 5 minutes count by their traffic.
// No bad event reads 0.
func badRatio(slo, window string) string {
	bad := "(sum(sum_over_time(" + sloSeries(SLOBadEvents, slo) + "[" + window + "])) or vector(0))"
	all := "sum(sum_over_time(" + sloSeries(SLOEvents, slo) + "[" + window + "]))"
	return bad + " / " + all
}

// budget is the error budget of an SLO, 1 - objective.
func budget(slo string) string {
	return "(1 - max(" + sloSeries(SLOObjective, slo) + "))"
}

// sloDashboard answers, per SLO over the chosen window (30 days by default):
// did the service meet the objective, how much error budget is left, how fast
// is it being spent right now, and how did the bad-event ratio move against
// the budget. The objectives come from the rule settings (vmafx:slo_objective),
// so the report shows what the chart or the Compose values set.
func sloDashboard() *dashboard.DashboardBuilder {
	b := newDashboard(SLOUID, "VMAFx SLO report",
		"Compliance with the SLO objectives over the dashboard's time range (30 days by default), the error budget left, and its burn rate. The objectives are the rule settings (monitoring.slo); the events are recorded every 5 minutes (vmafx:slo_events:rate5m, vmafx:slo_bad_events:rate5m). Generated from pkg/observability/metricdef by tools/obsgen.",
		m.BuildInfo).Time("now-30d", "now")
	var rows []row
	for _, s := range sloReports {
		rows = append(rows, sloRow(s))
	}
	return withRows(b, rows)
}

// sloRow is the four panels of one SLO.
func sloRow(s sloReport) row {
	objective := "max(" + sloSeries(SLOObjective, s.label) + ")"
	return row{s.title, []panelBuilder{
		statPanel(s.title+": compliance", "Share of "+s.good+" over the time range. Compare it with the objective next to it.", unitRatio,
			steps("blue"), instant("1 - "+badRatio(s.label, "$__range"), "compliance")).Span(6),
		statPanel(s.title+": objective", "The objective of the rule settings: the share of "+s.good+" the SLO promises over 30 days.", unitRatio,
			steps("blue"), instant(objective, "objective")).Span(6),
		statPanel(s.title+": error budget left", "Share of the error budget (1 - objective) the time range has left: 100 % is untouched, 0 % spent, below 0 the objective is missed.", unitRatio,
			steps("red", at(0, "orange"), at(0.25, "green")),
			instant("1 - ("+badRatio(s.label, "$__range")+") / "+budget(s.label), "budget left")).Span(6),
		statPanel(s.title+": burn rate, last hour", "How many times faster than the objective allows the last hour spent the budget: 1 spends it exactly over 30 days; the fast burn alert fires at 14.4 by default.", unitCount,
			steps("green", at(1, "orange"), at(6, "red")),
			instant("("+badRatio(s.label, "1h")+") / "+budget(s.label), "burn rate")).Span(6),
		timeseriesPanel(s.title+": bad events against the budget", "Hourly share of bad events (not "+s.good+") against the error budget: a curve above the budget line spends it faster than 30 days.", unitRatio,
			query(badRatio(s.label, "1h"), "bad events"),
			query(budget(s.label), "error budget")).Span(24),
	}}
}

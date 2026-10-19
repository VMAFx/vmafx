// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package obsgen

import (
	"github.com/grafana/grafana-foundation-sdk/go/dashboard"

	m "github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

// CapacityUID is the capacity dashboard's uid.
const CapacityUID = "vmafx-capacity"

// measuredCapacity is the jobs per hour the nodes' slots can finish at the
// mean job run time of the last 6 hours, measured on the nodes.
func measuredCapacity() string {
	meanRunTime := "(sum(increase(" + sel(m.NodeJobDuration.Name+"_sum") + "[6h])) / sum(increase(" + sel(m.NodeJobDuration.Name+"_count") + "[6h])))"
	return "sum(" + fam(m.NodeSlots) + ") * 3600 / " + meanRunTime
}

// demand is the jobs submitted to the controller in the last hour.
func demand() string {
	return "sum(increase(" + fam(m.ControllerJobsSubmitted) + "[1h]))"
}

// capacity answers: how many jobs per hour can the nodes finish at their
// measured run time, how many arrive, how much headroom is left, where the
// queue and the demand are heading (predict_linear over recent history), and
// how fast the demand grows.
func capacity() *dashboard.DashboardBuilder {
	b := newDashboard(CapacityUID, "VMAFx Capacity",
		"Capacity measured from the nodes (slots times the mean job run time of the last 6 hours) against demand (jobs submitted per hour), the headroom between them, and linear forecasts of the queue and the demand. Generated from pkg/observability/metricdef by tools/obsgen.",
		m.BuildInfo)
	growthPerDay := "deriv((" + demand() + ")[1d:1m]) * 86400"
	pending := orZero("sum(" + fam(m.ControllerJobsPending) + ")")
	return withRows(b, []row{
		{"Now", []panelBuilder{
			statPanel("Measured capacity", "Jobs per hour the nodes can finish: their slots times 3600 over the mean job run time of the last 6 hours.", unitCount,
				steps("blue"), instant(measuredCapacity(), "jobs per hour")).Span(6),
			statPanel("Demand", "Jobs submitted to the controller in the last hour.", unitCount,
				steps("blue"), instant(demand(), "jobs per hour")).Span(6),
			statPanel("Headroom", "1 - demand / capacity: the share of the capacity the demand leaves free. Below 0 the queue grows.", unitRatio,
				steps("red", at(0, "orange"), at(0.2, "green")),
				instant("1 - ("+demand()+") / ("+measuredCapacity()+")", "headroom")).Span(6),
			statPanel("Demand growth per day", "How much the hourly demand grew per day over the last day (linear fit): with the headroom, how soon demand meets capacity. Negative is shrinking.", unitCount,
				steps("green", at(0, "orange")),
				instant(growthPerDay, "jobs per hour per day")).Span(6),
		}},
		{"Forecast", []panelBuilder{
			timeseriesPanel("Pending jobs, forecast 24 hours", "Jobs waiting in the queue, and where the trend of the last 6 hours puts them in 24 hours. A rising forecast with headroom left points at a backend no node serves.", unitCount,
				query(pending, "pending"),
				query("predict_linear(("+pending+")[6h:1m], 86400)", "pending in 24 h")),
			timeseriesPanel("Demand, forecast 7 days, against capacity", "Jobs submitted per hour, the trend of the last day projected 7 days ahead, and the measured capacity.", unitCount,
				query(demand(), "demand"),
				query("predict_linear(("+demand()+")[1d:1m], 7 * 86400)", "demand in 7 days"),
				query(measuredCapacity(), "capacity")),
		}},
	})
}

// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package obsgen

import (
	"github.com/grafana/grafana-foundation-sdk/go/dashboard"

	m "github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

// LiveUID is the Live sessions dashboard's uid.
const LiveUID = "vmafx-live"

// live answers, for the ScoreStream sessions (ADR-0933) of vmafx-server and
// vmafx-node: how many are open and where, how many frames per second they
// move, how they end and how long they last.
func live() *dashboard.DashboardBuilder {
	b := newDashboard(LiveUID, "VMAFx Live sessions",
		"Live per-frame scoring over ScoreStream on vmafx-server and vmafx-node: open sessions, frame rate, outcomes and duration. Generated from pkg/observability/metricdef by tools/obsgen.",
		m.StreamSessions)
	return withRows(b, []row{
		{"Sessions now", []panelBuilder{
			statPanel("Open sessions", "ScoreStream sessions open now across the selected instances.", unitCount, steps("green"),
				instant(orZero("sum("+fam(m.StreamSessions)+")"), "open")).Span(6),
			statPanel("Frames per second", "Frame pairs received per second across the open sessions.", unitCount, steps("blue"),
				instant(rate(m.StreamFrames, ""), "fps")).Span(6),
			statPanel("Failed sessions", "Share of the sessions that ended in the window and failed (cancelled by the client not counted).", unitRatio,
				steps("green", at(0.01, "orange"), at(0.05, "red")),
				instant(rate(m.StreamSessionsFinished, "", `outcome="failed"`)+" / clamp_min("+rate(m.StreamSessionsFinished, "")+", 1e-9)", "failed")).Span(6),
			statPanel("Median session length", "Median duration of the sessions that ended in the window.", unitSeconds, steps("blue"),
				instant(quantile(0.5, m.StreamSessionDuration, ""), "p50")).Span(6),
		}},
		{"Over time", []panelBuilder{
			timeseriesPanel("Open sessions by instance", "Sessions open on each server or node. All on one instance means the clients do not spread.", unitCount,
				query("sum by (instance) ("+fam(m.StreamSessions)+")", "{{instance}}")),
			timeseriesPanel("Frames per second by instance", "Frame pairs received per second on each instance: the scoring throughput of the live path.", unitCount,
				query(rate(m.StreamFrames, "instance"), "{{instance}}")),
			timeseriesPanel("Sessions ended per minute by outcome", "Sessions that completed, failed or were cancelled by the client, per minute.", unitPerMin,
				query(rate(m.StreamSessionsFinished, "outcome")+" * 60", "{{outcome}}")),
			timeseriesPanel("Session duration", "Duration of the sessions that ended: median and 95th percentile. The dots are exemplars: open one for the trace of that session.", unitSeconds,
				queryExemplars(quantile(0.5, m.StreamSessionDuration, ""), "p50"),
				queryExemplars(quantile(0.95, m.StreamSessionDuration, ""), "p95")),
		}},
	})
}

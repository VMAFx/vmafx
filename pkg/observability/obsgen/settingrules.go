// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package obsgen

import m "github.com/VMAFx/vmafx/pkg/observability/metricdef"

// Recorded series of the settings (decisions Q-193, Q-195): the dashboards
// read the configured objectives and prices from Prometheus, so they show
// what the chart or the Compose values set.
const (
	SLOObjective   = "vmafx:slo_objective"
	SLOEvents      = "vmafx:slo_events:rate5m"
	SLOBadEvents   = "vmafx:slo_bad_events:rate5m"
	PriceJobSecond = "vmafx:price_job_second"
	PricePerJob    = "vmafx:price_job"
)

// SLO label values of SLOObjective, by settings key.
var sloLabel = map[string]string{
	"jobSuccess":   "job_success",
	"scoreSuccess": "score_success",
	"scoreLatency": "score_latency",
}

// perScrapedInstance writes a constant as one sample per scraped VMAFx
// instance, carrying its job and instance: a dashboard selects it with the
// job and instance matchers every query carries (dashboard-linter's
// target-job-rule and target-instance-rule), and it exists wherever a VMAFx
// component is scraped.
func perScrapedInstance(value string) string {
	return "max by (job, instance) (" + m.BuildInfo.Name + ") * 0 + " + value
}

// sloEventRules record, per SLO and scraped instance, the rate of all events
// and of bad events over 5 minutes; the SLO report adds them up over its
// window, which weights every 5 minutes by its traffic. The latency SLO's
// bucket bound is a setting.
func sloEventRules(p params) []recordingRule {
	byInstance := func(series string) string {
		return "sum by (job, instance) (rate(" + series + "[5m]))"
	}
	failed, done := byInstance(m.ControllerJobsFailed.Name), byInstance(m.ControllerJobsCompleted.Name)
	slow := byInstance(m.ServerScoreDuration.Name+"_count") + " - " +
		byInstance(m.ServerScoreDuration.Name+"_bucket{"+LeMatcher(p.latencyLE)+"}")
	events := []struct{ slo, all, bad string }{
		{"job_success", "(" + failed + " + " + done + ") or " + done + " or " + failed, failed},
		{"score_success", byInstance(m.ServerScoreRequests.Name), byInstance(m.ServerScoreErrors.Name)},
		{"score_latency", byInstance(m.ServerScoreDuration.Name + "_count"), slow},
	}
	var out []recordingRule
	for _, e := range events {
		out = append(out,
			recordingRule{Record: SLOEvents, Expr: e.all, Labels: map[string]string{"slo": e.slo}},
			recordingRule{Record: SLOBadEvents, Expr: e.bad, Labels: map[string]string{"slo": e.slo}})
	}
	return out
}

// settingRecordingRules record each SLO objective under the label slo and
// each price under the label currency. A price rule keeps only a positive
// price, so an unset price (0) records nothing and its panels stay empty.
func settingRecordingRules(p params) []recordingRule {
	out := sloEventRules(p)
	for _, key := range []string{"jobSuccess", "scoreSuccess", "scoreLatency"} {
		out = append(out, recordingRule{
			Record: SLOObjective, Expr: perScrapedInstance(p.objective(key)),
			Labels: map[string]string{"slo": sloLabel[key]},
		})
	}
	for _, price := range []struct{ record, value string }{
		{PriceJobSecond, p.pricePerJobSecond}, {PricePerJob, p.pricePerJob},
	} {
		out = append(out, recordingRule{
			Record: price.record, Expr: "(" + perScrapedInstance(price.value) + ") > 0",
			Labels: map[string]string{"currency": p.currency},
		})
	}
	return out
}

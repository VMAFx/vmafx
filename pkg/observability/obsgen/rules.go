// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package obsgen

import (
	"fmt"
	"strings"

	m "github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

// RulesFile is the generated Prometheus rule file; RulesTestFile is its
// promtool unit test, which reads it by the base name.
const (
	RulesFile     = "deploy/prometheus/vmafx-rules.yaml"
	RulesTestFile = "deploy/prometheus/vmafx-rules.test.yaml"
)

// RunbookBase is where the runbook pages are published: an alert's
// runbook_url is RunbookBase + its runbook slug + "/". The pages are
// docs/observability/runbooks/<slug>.md.
const RunbookBase = "https://vmafx.github.io/vmafx/observability/runbooks/"

// Service level objectives the burn-rate alerts guard. Each is the share of
// events that must be good over 30 days; the error budget is 1 - objective.
const (
	// JobSuccessObjective: controller jobs that complete rather than fail.
	JobSuccessObjective = 0.99
	// ScoreSuccessObjective: Score requests that do not return an error.
	ScoreSuccessObjective = 0.99
	// ScoreLatencyObjective: Score requests that finish within
	// ScoreLatencyThreshold seconds.
	ScoreLatencyObjective = 0.99
	// ScoreLatencyThreshold is a bucket bound of
	// vmafx_server_score_duration_seconds (metricdef.RequestSecondsBuckets).
	ScoreLatencyThreshold = "30"
)

// Burn-rate windows (multi-window, multi-burn-rate alerting): a fast burn
// spends 2 % of a 30-day budget in an hour, a slow one 5 % in six hours; each
// needs its long and its short window above the rate, so an alert stops soon
// after the burn does.
var burnWindows = []struct {
	long, short, forDur, severity string
	factor                        float64
}{
	{"1h", "5m", "2m", "critical", 14.4},
	{"6h", "30m", "15m", "warning", 6},
}

// sloRatio is one bad-event ratio recorded per window for a burn-rate alert.
type sloRatio struct {
	record    string // recording rule name without the window suffix
	objective float64
	expr      func(window string) string
}

// sloRatios are the ratios of bad events the burn-rate alerts read.
func sloRatios() []sloRatio {
	return []sloRatio{
		{"vmafx:job_failure_ratio", JobSuccessObjective, func(w string) string {
			failed := "sum(rate(" + m.ControllerJobsFailed.Name + "[" + w + "]))"
			done := "sum(rate(" + m.ControllerJobsCompleted.Name + "[" + w + "]))"
			return failed + " / (" + failed + " + " + done + ")"
		}},
		{"vmafx:score_error_ratio", ScoreSuccessObjective, func(w string) string {
			return "sum(rate(" + m.ServerScoreErrors.Name + "[" + w + "])) / sum(rate(" + m.ServerScoreRequests.Name + "[" + w + "]))"
		}},
		{"vmafx:score_slow_ratio", ScoreLatencyObjective, func(w string) string {
			fast := "sum(rate(" + m.ServerScoreDuration.Name + "_bucket{" + LeMatcher(ScoreLatencyThreshold) + "}[" + w + "]))"
			all := "sum(rate(" + m.ServerScoreDuration.Name + "_count[" + w + "]))"
			return "1 - " + fast + " / " + all
		}},
	}
}

// recordingRule is one Prometheus recording rule.
type recordingRule struct {
	Record string `yaml:"record"`
	Expr   string `yaml:"expr"`
}

// recordingRules are the SLO ratios per window and the hourly quality
// summaries the score regression alert reads.
func recordingRules() []recordingRule {
	var out []recordingRule
	for _, r := range sloRatios() {
		for _, w := range []string{"5m", "30m", "1h", "6h"} {
			out = append(out, recordingRule{r.record + ":rate" + w, r.expr(w)})
		}
	}
	return append(out,
		recordingRule{"vmafx:quality_score:p50_1h",
			"histogram_quantile(0.5, sum by (tenant, model, le) (rate(" + m.QualityScore.Name + "_bucket[1h])))"},
		recordingRule{"vmafx:quality_score:count_1h",
			"sum by (tenant, model) (increase(" + m.QualityScore.Name + "_count[1h]))"},
	)
}

// RecordedSeries are the series the rule file records; a dashboard may query
// them like the families of metricdef.
func RecordedSeries() []string {
	var out []string
	for _, r := range recordingRules() {
		out = append(out, r.Record)
	}
	return out
}

// alert is one alert name: its rules (one per severity), and the promtool
// cases that prove it, at least one where it fires and one where it does not.
type alert struct {
	name, runbook string
	rules         []alertRule
	cases         []alertCase
}

// alertRule is one rule of an alert name. Annotations may use
// {{ $labels.<name> }} only, which the test file renders the same way.
type alertRule struct {
	expr, forDur, severity string
	summary, description   string
}

// alertCase is one promtool alert test: input series at interval, the
// evaluation time, and the label sets of the alerts expected to fire, each
// with its severity (none for a negative case).
type alertCase struct {
	interval string
	series   []inputSeries
	evalTime string
	firing   []map[string]string
}

// inputSeries is one promtool input series in its expanding notation.
type inputSeries struct {
	Series string `yaml:"series"`
	Values string `yaml:"values"`
}

// burnRules builds the fast and the slow rule of one SLO ratio.
func burnRules(what string, r sloRatio) []alertRule {
	budget := 1 - r.objective
	var out []alertRule
	for _, w := range burnWindows {
		threshold := fmt.Sprintf("%.6g", w.factor*budget)
		out = append(out, alertRule{
			forDur: w.forDur, severity: w.severity,
			expr:    r.record + ":rate" + w.long + " > " + threshold + " and " + r.record + ":rate" + w.short + " > " + threshold,
			summary: fmt.Sprintf("%s is spending the error budget %g times faster than the SLO allows.", what, w.factor),
			description: fmt.Sprintf("Over the last %s and %s the %s ratio exceeded %s (SLO %g %%, burn rate %g).",
				w.long, w.short, strings.ToLower(what), threshold, r.objective*100, w.factor),
		})
	}
	return out
}

// runbookURL is the published page of a runbook slug.
func runbookURL(slug string) string {
	return RunbookBase + slug + "/"
}

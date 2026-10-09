// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package obsgen

import (
	"fmt"
	"slices"
	"strconv"
	"strings"

	m "github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

// RulesFile is the generated Prometheus rule file (default settings);
// RulesTestFile is its promtool unit test, which reads it by the base name.
const (
	RulesFile     = "deploy/prometheus/vmafx-rules.yaml"
	RulesTestFile = "deploy/prometheus/vmafx-rules.test.yaml"
)

// RunbookBase is where the runbook pages are published: an alert's
// runbook_url is RunbookBase + its runbook slug + "/". The pages are
// docs/observability/runbooks/<slug>.md.
const RunbookBase = "https://vmafx.dev/observability/runbooks/"

// params renders the tunable parts of the rules: the values of a Settings
// for a rule file (plainParams), Helm expressions over .Values.monitoring for
// the chart's PrometheusRule template (helmParams). Both renderings build the
// same rules from the same code, and a threshold is the PromQL expression of
// the values, never a number computed here, so the chart and a rule file
// rendered from the same values agree to the character.
type params struct {
	// windows are the windows the SLO ratios are recorded over.
	windows []string
	burns   []burnParams
	// objective is the objective of an SLO key (jobSuccess, scoreSuccess,
	// scoreLatency).
	objective func(key string) string
	// latencyLE is the le bound of the latency objective.
	latencyLE                                 string
	queueAge, regressionPoints, regressionMin string
	// label references a Prometheus label in an annotation.
	label func(name string) string
	// The prices and the currency of the cost dashboard.
	pricePerJobSecond, pricePerJob, currency string
}

// burnParams is one burn-rate rule of params.
type burnParams struct {
	long, short, forDur, severity, factor string
}

// plainParams renders s. Numbers are printed as Helm prints the same values
// (fmt.Sprint of a float64, integers through int).
func plainParams(s Settings) params {
	objectives := map[string]float64{"jobSuccess": s.SLO.JobSuccess, "scoreSuccess": s.SLO.ScoreSuccess, "scoreLatency": s.SLO.ScoreLatency}
	burn := func(severity string, b BurnRate) burnParams {
		return burnParams{b.LongWindow, b.ShortWindow, b.For, severity, fmt.Sprint(b.Factor)}
	}
	p := params{
		burns:             []burnParams{burn("critical", s.BurnRates.Fast), burn("warning", s.BurnRates.Slow)},
		objective:         func(key string) string { return fmt.Sprint(objectives[key]) },
		latencyLE:         s.SLO.ScoreLatencySeconds,
		queueAge:          strconv.Itoa(s.Alerts.QueueAgeSeconds),
		regressionPoints:  fmt.Sprint(s.Alerts.ScoreRegressionPoints),
		regressionMin:     strconv.Itoa(s.Alerts.ScoreRegressionMinScores),
		label:             func(name string) string { return "{{ $labels." + name + " }}" },
		pricePerJobSecond: fmt.Sprint(s.Cost.PerJobSecond),
		pricePerJob:       fmt.Sprint(s.Cost.PerJob),
		currency:          s.Cost.Currency,
	}
	for _, b := range p.burns {
		for _, w := range []string{b.long, b.short} {
			if !slices.Contains(p.windows, w) {
				p.windows = append(p.windows, w)
			}
		}
	}
	return p
}

// helmValues is the values path of the settings in the chart, from the root
// ($) so it resolves inside the range over the windows too.
const helmValues = "$.Values.monitoring"

// helmWindow is the variable the chart's template ranges the windows with;
// helmWindows is the list it ranges over: the four windows of the two burn
// rates, in the order plainParams collects them, without repeats.
const (
	helmWindow  = "{{ $w }}"
	helmWindows = "uniq (list " + helmValues + ".burnRates.fast.longWindow " + helmValues + ".burnRates.fast.shortWindow " +
		helmValues + ".burnRates.slow.longWindow " + helmValues + ".burnRates.slow.shortWindow)"
)

// helmParams renders the settings as Helm expressions; the Prometheus label
// references are escaped so Helm leaves them for Prometheus.
func helmParams() params {
	value := func(path string) string { return "{{ " + helmValues + "." + path + " }}" }
	burn := func(key, severity string) burnParams {
		v := "burnRates." + key
		return burnParams{value(v + ".longWindow"), value(v + ".shortWindow"), value(v + ".for"), severity, value(v + ".factor")}
	}
	return params{
		windows:           []string{helmWindow},
		burns:             []burnParams{burn("fast", "critical"), burn("slow", "warning")},
		objective:         func(key string) string { return value("slo." + key) },
		latencyLE:         value("slo.scoreLatencySeconds"),
		queueAge:          "{{ " + helmValues + ".alerts.queueAgeSeconds | int }}",
		regressionPoints:  value("alerts.scoreRegressionPoints"),
		regressionMin:     "{{ " + helmValues + ".alerts.scoreRegressionMinScores | int }}",
		label:             func(name string) string { return "{{`{{ $labels." + name + " }}`}}" },
		pricePerJobSecond: value("cost.perJobSecond"),
		pricePerJob:       value("cost.perJob"),
		currency:          value("cost.currency"),
	}
}

// sloRatio is one bad-event ratio recorded per window for a burn-rate alert.
type sloRatio struct {
	record, objectiveKey string
	expr                 func(window string) string
}

// sloRatios are the ratios of bad events the burn-rate alerts read.
func sloRatios(p params) []sloRatio {
	return []sloRatio{
		{"vmafx:job_failure_ratio", "jobSuccess", func(w string) string {
			failed := "sum(rate(" + m.ControllerJobsFailed.Name + "[" + w + "]))"
			done := "sum(rate(" + m.ControllerJobsCompleted.Name + "[" + w + "]))"
			return failed + " / (" + failed + " + " + done + ")"
		}},
		{"vmafx:score_error_ratio", "scoreSuccess", func(w string) string {
			return "sum(rate(" + m.ServerScoreErrors.Name + "[" + w + "])) / sum(rate(" + m.ServerScoreRequests.Name + "[" + w + "]))"
		}},
		{"vmafx:score_slow_ratio", "scoreLatency", func(w string) string {
			fast := "sum(rate(" + m.ServerScoreDuration.Name + "_bucket{" + LeMatcher(p.latencyLE) + "}[" + w + "]))"
			all := "sum(rate(" + m.ServerScoreDuration.Name + "_count[" + w + "]))"
			return "1 - " + fast + " / " + all
		}},
	}
}

// recordingRule is one Prometheus recording rule.
type recordingRule struct {
	Record string            `yaml:"record"`
	Expr   string            `yaml:"expr"`
	Labels map[string]string `yaml:"labels,omitempty"`
}

// sloRecordingRules are the SLO ratios over every window of p.
func sloRecordingRules(p params) []recordingRule {
	var out []recordingRule
	for _, r := range sloRatios(p) {
		for _, w := range p.windows {
			out = append(out, recordingRule{Record: r.record + ":rate" + w, Expr: r.expr(w)})
		}
	}
	return out
}

// qualityRecordingRules are the hourly quality summaries the score
// regression alert reads; they take no settings.
func qualityRecordingRules() []recordingRule {
	return []recordingRule{
		{Record: "vmafx:quality_score:p50_1h",
			Expr: "histogram_quantile(0.5, sum by (tenant, model, le) (rate(" + m.QualityScore.Name + "_bucket[1h])))"},
		{Record: "vmafx:quality_score:count_1h",
			Expr: "sum by (tenant, model) (increase(" + m.QualityScore.Name + "_count[1h]))"},
	}
}

// recordingRules are every recording rule, in the order of the rule file:
// the SLO ratios, the quality summaries, then the settings.
func recordingRules(p params) []recordingRule {
	out := append(sloRecordingRules(p), qualityRecordingRules()...)
	return append(out, settingRecordingRules(p)...)
}

// RecordedSeries are the series the rule file records with the default
// settings; a dashboard may query them like the families of metricdef.
func RecordedSeries() []string {
	var out []string
	for _, r := range recordingRules(plainParams(DefaultSettings())) {
		if !slices.Contains(out, r.Record) {
			out = append(out, r.Record)
		}
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

// alertRule is one rule of an alert name. Annotations reference labels
// through params.label only, which the test file renders the same way.
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

// burnRules builds the fast and the slow rule of one SLO ratio: the ratio
// over both windows of a burn above factor times the error budget.
func burnRules(p params, what string, r sloRatio) []alertRule {
	var out []alertRule
	for _, b := range p.burns {
		threshold := "(" + b.factor + " * (1 - " + p.objective(r.objectiveKey) + "))"
		out = append(out, alertRule{
			forDur: b.forDur, severity: b.severity,
			expr:    r.record + ":rate" + b.long + " > " + threshold + " and " + r.record + ":rate" + b.short + " > " + threshold,
			summary: what + " is spending the error budget " + b.factor + " times faster than the SLO allows.",
			description: "Over the last " + b.long + " and " + b.short + " the " + strings.ToLower(what) + " ratio exceeded " +
				b.factor + " times the error budget of the objective " + p.objective(r.objectiveKey) + ".",
		})
	}
	return out
}

// runbookURL is the published page of a runbook slug.
func runbookURL(slug string) string {
	return RunbookBase + slug + "/"
}

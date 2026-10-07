// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package obsgen

import (
	"github.com/grafana/grafana-foundation-sdk/go/common"
	"github.com/grafana/grafana-foundation-sdk/go/dashboard"
	"github.com/grafana/grafana-foundation-sdk/go/heatmap"
	"github.com/grafana/grafana-foundation-sdk/go/prometheus"

	m "github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

// QualityUID is the Quality dashboard's uid.
const QualityUID = "vmafx-quality"

// qualityMatchers select the dashboard's tenants, models and profiles.
var qualityMatchers = []string{tenantMatcher, `model=~"$model"`, `profile=~"$profile"`}

// quality answers: did the scores move (per model, per tenant), how wide is
// the spread, how many fall below a usable level, and how many scores were
// produced at all.
func quality() *dashboard.DashboardBuilder {
	b := newDashboard(QualityUID, "VMAFx Quality",
		"Distribution of the pooled VMAF scores of completed jobs and Score requests, per tenant, model and profile. A shift without a change of content points at an encoder, a pipeline or a model regression. Generated from pkg/observability/metricdef by tools/obsgen.",
		m.BuildInfo, tenantVariable(),
		scopeVariable("model", "label_values("+m.QualityScore.Name+`_count{job=~"$job",tenant=~"$tenant"}, model)`),
		scopeVariable("profile", "label_values("+m.QualityScore.Name+`_count{job=~"$job"}, profile)`))
	return withRows(b, []row{
		{"Score levels", []panelBuilder{
			timeseriesPanel("Median score by model", "Median pooled score per model over the window.", unitScore,
				query(quantile(0.5, m.QualityScore, "model", qualityMatchers...), "{{model}}")),
			timeseriesPanel("10th percentile score by model", "The score 90 percent of the results stay above, per model: the bad tail a median hides.", unitScore,
				query(quantile(0.1, m.QualityScore, "model", qualityMatchers...), "{{model}}")),
			timeseriesPanel("Median score by tenant", "Median pooled score per tenant. A drop for one tenant alone points at its content or its encodes, not at the platform.", unitScore,
				query(quantile(0.5, m.QualityScore, "tenant", qualityMatchers...), "{{tenant}}")),
			timeseriesPanel("Share of scores below 70", "Scores below 70 over all scores, per model. 70 is a common floor for acceptable streaming quality; the buckets also hold 60, 75 and 80.", unitRatio,
				query(sumBy("model", "rate("+bucket(m.QualityScore, append(qualityMatchers, `le="70"`)...)+rateWindow+")")+
					" / clamp_min("+sumBy("model", "rate("+sel(m.QualityScore.Name+"_count", qualityMatchers...)+rateWindow+")")+", 1e-9)", "{{model}}")),
		}},
		{"Distribution and volume", []panelBuilder{
			scoreHeatmap(),
			timeseriesPanel("Scores per minute by model", "Number of scores produced per minute, per model: the volume behind the levels above.", unitPerMin,
				query(sumBy("model", "rate("+sel(m.QualityScore.Name+"_count", qualityMatchers...)+rateWindow+")")+" * 60", "{{model}}")),
		}},
	})
}

// scoreHeatmap shows how the scores spread over the buckets in each interval.
func scoreHeatmap() *heatmap.PanelBuilder {
	return heatmap.NewPanelBuilder().
		Title("Score distribution").
		Description("Scores per bucket over time for the selection. A band moving down, or a second band appearing, is a regression of part of the work.").
		Datasource(promRef()).
		Unit(unitScore).
		Height(8).
		Span(12).
		Calculate(false).
		Color(heatmap.NewHeatmapColorOptionsBuilder().Mode(heatmap.HeatmapColorModeScheme).Scheme("Oranges").Steps(64)).
		YAxis(heatmap.NewYAxisConfigBuilder().Unit(unitScore)).
		CellGap(1).
		Tooltip(heatmap.NewHeatmapTooltipBuilder().Mode(common.TooltipDisplayModeSingle).YHistogram(true)).
		WithTarget(prometheus.NewDataqueryBuilder().
			Datasource(promRef()).
			Expr(sumBy("le", "increase("+bucket(m.QualityScore, qualityMatchers...)+rateWindow+")")).
			Format(prometheus.PromQueryFormatHeatmap).
			LegendFormat("{{le}}").
			Range())
}

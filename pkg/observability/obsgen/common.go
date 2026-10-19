// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package obsgen

import (
	"github.com/grafana/grafana-foundation-sdk/go/cog"
	"github.com/grafana/grafana-foundation-sdk/go/cog/variants"
	"github.com/grafana/grafana-foundation-sdk/go/common"
	"github.com/grafana/grafana-foundation-sdk/go/dashboard"
	"github.com/grafana/grafana-foundation-sdk/go/prometheus"
	"github.com/grafana/grafana-foundation-sdk/go/stat"
	"github.com/grafana/grafana-foundation-sdk/go/timeseries"

	"github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

// promDatasourceVar is the dashboards' Prometheus data source variable,
// named for its type so a Loki variable can sit next to it
// (dashboard-linter's template-datasource-rule).
const promDatasourceVar = "prometheus_datasource"

// tag is on every generated dashboard; the dashboard links list them by it.
const tag = "vmafx"

// promRef is the panel and variable data source: the template variable.
func promRef() common.DataSourceRef {
	return common.DataSourceRef{Type: new("prometheus"), Uid: new("${" + promDatasourceVar + "}")}
}

// newDashboard starts a generated VMAFx dashboard: baseDashboard over the
// series of scope, plus the deploy annotation.
func newDashboard(uid, title, description string, scope metricdef.Family, extra ...*dashboard.QueryVariableBuilder) *dashboard.DashboardBuilder {
	return baseDashboard(uid, title, description, scope.Name, []string{tag}, extra...).Annotation(deployAnnotation())
}

// baseDashboard is read-only, carries tags, the shared variables (data
// source, then job and instance over scopeSeries, then extra) and a link to
// the other VMAFx dashboards.
func baseDashboard(uid, title, description, scopeSeries string, tags []string, extra ...*dashboard.QueryVariableBuilder) *dashboard.DashboardBuilder {
	b := dashboard.NewDashboardBuilder(title).
		Uid(uid).
		Description(description).
		Tags(tags).
		Readonly().
		Tooltip(dashboard.DashboardCursorSyncCrosshair).
		Refresh("1m").
		Time("now-6h", "now").
		Timezone(common.TimeZoneBrowser).
		WithVariable(dashboard.NewDatasourceVariableBuilder(promDatasourceVar).
			Label("Prometheus data source").
			Type("prometheus")).
		WithVariable(scopeVariable("job", "label_values("+scopeSeries+", job)")).
		WithVariable(scopeVariable("instance", "label_values("+scopeSeries+`{job=~"$job"}, instance)`)).
		Link(dashboard.NewDashboardLinkBuilder("VMAFx dashboards").
			Type(dashboard.DashboardLinkTypeDashboards).
			Tags([]string{tag}).
			AsDropdown(true).
			IncludeVars(true).
			KeepTime(true))
	for _, v := range extra {
		b = b.WithVariable(v)
	}
	return b
}

// scopeVariable is a multi-value query variable over the label of the query,
// whose All selects every value (allValue ".+", dashboard-linter's
// template-job-rule and template-instance-rule).
func scopeVariable(name, query string) *dashboard.QueryVariableBuilder {
	return dashboard.NewQueryVariableBuilder(name).
		Label(titleCase(name)).
		Datasource(promRef()).
		Query(dashboard.StringOrMap{String: new(query)}).
		Definition(query).
		Refresh(dashboard.VariableRefreshOnTimeRangeChanged).
		Sort(dashboard.VariableSortAlphabeticalAsc).
		Multi(true).
		IncludeAll(true).
		AllValue(".+")
}

// tenantVariable selects tenants among those that submitted jobs.
func tenantVariable() *dashboard.QueryVariableBuilder {
	return scopeVariable("tenant", "label_values("+metricdef.ControllerJobsSubmitted.Name+`{job=~"$job"}, tenant)`)
}

func titleCase(s string) string {
	if s == "" {
		return s
	}
	return string(s[0]-'a'+'A') + s[1:]
}

// deployAnnotation marks each process start with the build version it runs:
// a rollout shows as one mark per restarted pod.
func deployAnnotation() *dashboard.AnnotationQueryBuilder {
	expr := "max by (job, instance, version) (" + sel(metricdef.BuildInfo.Name) + ")" +
		" and on (job, instance) changes(" + sel("process_start_time_seconds") + "[5m]) > 0"
	return dashboard.NewAnnotationQueryBuilder().
		Name("Deploys and restarts").
		Datasource(promRef()).
		Enable(true).
		IconColor("blue").
		Expr(expr).
		Step("1m").
		TitleFormat("{{job}} {{version}} started").
		TextFormat("{{instance}}")
}

// query is one Prometheus target.
func query(expr, legend string) *prometheus.DataqueryBuilder {
	return prometheus.NewDataqueryBuilder().Datasource(promRef()).Expr(expr).LegendFormat(legend).Range()
}

// instant is one Prometheus target evaluated at the end of the range.
func instant(expr, legend string) *prometheus.DataqueryBuilder {
	return prometheus.NewDataqueryBuilder().Datasource(promRef()).Expr(expr).LegendFormat(legend).Instant()
}

// statPanel is a single-number panel; thresholds are base color then steps.
func statPanel(title, description, unit string, thresholds []dashboard.Threshold, targets ...cog.Builder[variants.Dataquery]) *stat.PanelBuilder {
	p := stat.NewPanelBuilder().
		Title(title).
		Description(description).
		Datasource(promRef()).
		Unit(unit).
		Height(4).
		Span(4).
		GraphMode(common.BigValueGraphModeArea).
		ColorMode(common.BigValueColorModeValue).
		ReduceOptions(common.NewReduceDataOptionsBuilder().Calcs([]string{"lastNotNull"})).
		Thresholds(dashboard.NewThresholdsConfigBuilder().Mode(dashboard.ThresholdsModeAbsolute).Steps(thresholds))
	for _, t := range targets {
		p = p.WithTarget(t)
	}
	return p
}

// timeseriesPanel is a time series panel with a table legend.
func timeseriesPanel(title, description, unit string, targets ...cog.Builder[variants.Dataquery]) *timeseries.PanelBuilder {
	p := timeseries.NewPanelBuilder().
		Title(title).
		Description(description).
		Datasource(promRef()).
		Unit(unit).
		Min(0).
		Height(8).
		Span(12).
		FillOpacity(10).
		Legend(common.NewVizLegendOptionsBuilder().
			DisplayMode(common.LegendDisplayModeTable).
			Placement(common.LegendPlacementBottom).
			ShowLegend(true).
			Calcs([]string{"mean", "max", "lastNotNull"})).
		Tooltip(common.NewVizTooltipOptionsBuilder().Mode(common.TooltipDisplayModeMulti).Sort(common.SortOrderDescending))
	for _, t := range targets {
		p = p.WithTarget(t)
	}
	return p
}

// drillDown is a panel link to another generated dashboard that keeps the
// time range and the variables.
func drillDown(title, uid string) []cog.Builder[dashboard.DashboardLink] {
	return []cog.Builder[dashboard.DashboardLink]{
		dashboard.NewDashboardLinkBuilder(title).
			Type(dashboard.DashboardLinkTypeLink).
			Url("/d/" + uid + "?${__url_time_range}&${__all_variables}"),
	}
}

// row is a titled group of panels.
type row struct {
	title  string
	panels []panelBuilder
}

// withRows adds each row and its panels to b.
func withRows(b *dashboard.DashboardBuilder, rows []row) *dashboard.DashboardBuilder {
	for _, r := range rows {
		b = b.WithRow(dashboard.NewRowBuilder(r.title))
		for _, p := range r.panels {
			b = b.WithPanel(p)
		}
	}
	return b
}

// steps builds thresholds: the base color, then the given steps.
func steps(base string, more ...dashboard.Threshold) []dashboard.Threshold {
	return append([]dashboard.Threshold{{Color: base}}, more...)
}

// at is the threshold step that turns color from value up.
func at(value float64, color string) dashboard.Threshold {
	return dashboard.Threshold{Value: new(value), Color: color}
}

// Grafana unit identifiers by metricdef unit.
const (
	unitSeconds = "s"
	unitCount   = "short"
	unitRatio   = "percentunit"
	unitPerMin  = "opm"
	unitScore   = "none"
)

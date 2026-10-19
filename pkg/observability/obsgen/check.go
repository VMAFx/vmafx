// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package obsgen

import (
	"encoding/json"
	"fmt"
	"regexp"
	"slices"
	"strconv"
	"strings"

	"go.yaml.in/yaml/v3"

	"github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

// ExternalSeries are the series a dashboard may query that VMAFx code does
// not define: Prometheus's own up series, and the Go runtime and process
// collectors every component registers (observability.NewRegistry). The
// collector entries are checked against a live registry by the tests, so the
// list cannot name a series nothing serves.
var ExternalSeries = map[string]string{
	"up":                            "Prometheus, one per scrape target",
	"process_start_time_seconds":    "client_golang process collector, every component",
	"process_resident_memory_bytes": "client_golang process collector, every component",
	"process_cpu_seconds_total":     "client_golang process collector, every component",
	"go_goroutines":                 "client_golang Go collector, every component",
}

// ExporterSeries are the series of the vendor GPU exporters, by exporter key,
// as each exporter's reference names them (verified 2026-10-07). They are
// allowed only on a dashboard tagged exporterTagPrefix+key: a VMAFx dashboard
// that queried one would show "No data" wherever that exporter is absent.
var ExporterSeries = map[string][]string{
	// NVIDIA dcgm-exporter, etc/default-counters.csv.
	"dcgm": {"DCGM_FI_DEV_GPU_UTIL", "DCGM_FI_DEV_FB_USED", "DCGM_FI_DEV_FB_FREE",
		"DCGM_FI_DEV_ENC_UTIL", "DCGM_FI_DEV_DEC_UTIL", "DCGM_FI_DEV_POWER_USAGE"},
	// AMD device-metrics-exporter, grafana/dashboard_gpu.json and docs/configuration/metricslist.md.
	"amd": {"gpu_gfx_activity", "gpu_used_vram", "gpu_total_vram", "gpu_package_power"},
	// Intel XPU Manager, doc/Prometheus_Exported_Metrics.csv.
	"intel": {"xpum_engine_ratio", "xpum_memory_ratio", "xpum_engine_group_ratio", "xpum_power_watts"},
}

// CheckExpr returns the series expr selects that nothing emits: neither a
// series of a metricdef family, nor one of ExternalSeries, nor a series the
// generated rule file records, nor a series of one of the exporters named in
// exporters.
func CheckExpr(expr string, exporters ...string) []string {
	var dead []string
	for _, name := range MetricNames(expr) {
		if !emitted(name, exporters) {
			dead = append(dead, name)
		}
	}
	return dead
}

// wholeNumberMatcher finds an equality matcher on le or quantile with a whole
// number: Prometheus 3 ingests those label values in float form (le="70" is
// stored as le="70.0"), so such a matcher selects nothing.
var wholeNumberMatcher = regexp.MustCompile(`\b(le|quantile)\s*(=|!=)\s*"([0-9]+)"`)

// CheckBucketMatchers returns one problem per equality matcher of expr on a
// whole-number le or quantile value; LeMatcher writes the matcher that
// selects the bucket on Prometheus 2 and 3.
func CheckBucketMatchers(expr string) []string {
	var out []string
	for _, mm := range wholeNumberMatcher.FindAllStringSubmatch(expr, -1) {
		out = append(out, fmt.Sprintf("%s matches nothing on Prometheus 3, which stores %s=%q; use %s",
			mm[0], mm[1], mm[3]+".0", LeMatcher(mm[3])))
	}
	return out
}

// The Grafana variables a dashboard query may hold: the time-range ones, and
// every other (template variables, whose "All" Instantiate selects).
var (
	rangeVariable = regexp.MustCompile(`\$(?:\{(?:__rate_interval|__interval|__range)\}|(?:__rate_interval|__interval|__range)\b)`)
	otherVariable = regexp.MustCompile(`\$\{?[A-Za-z_][A-Za-z0-9_]*\}?`)
)

// Instantiate turns a dashboard query into the query Grafana sends with every
// template variable set to "All" (".*") and the time-range variables set to
// window. The Compose smoke test and the promtool cases of the dashboards'
// queries evaluate dashboard queries through it.
func Instantiate(expr, window string) string {
	return otherVariable.ReplaceAllString(rangeVariable.ReplaceAllString(expr, window), ".*")
}

// LeMatcher selects the bucket with upper bound bound (as the client writes
// it, "70" or "0.05") whether Prometheus stored it as written (Prometheus 2,
// text format) or in float form (Prometheus 3: "70.0").
// TestLeMatcherSelectsOneBucket proves it selects no other bucket of any
// histogram family.
func LeMatcher(bound string) string {
	return `le=~"` + bound + `(\\.0)?"`
}

// bucketBound is a bucket's upper bound as the Prometheus client writes it.
func bucketBound(b float64) string {
	return strconv.FormatFloat(b, 'g', -1, 64)
}

func emitted(name string, exporters []string) bool {
	if _, ok := metricdef.Lookup(name); ok {
		return true
	}
	if _, ok := ExternalSeries[name]; ok {
		return true
	}
	if slices.Contains(RecordedSeries(), name) {
		return true
	}
	for _, e := range exporters {
		if slices.Contains(ExporterSeries[e], name) {
			return true
		}
	}
	return false
}

// Query is one query of a dashboard: where it sits (a panel, an annotation or
// a query variable, with its title or name), its expression, and the type of
// its data source ("prometheus", "loki"; empty for the panel's default).
type Query struct {
	Where, Expr, Datasource string
}

// Dashboard is what DashboardQueries reads from a dashboard's JSON.
type Dashboard struct {
	Title string
	// Exporters are the exporter keys the dashboard's tags name.
	Exporters []string
	Queries   []Query
}

// DashboardQueries parses a Grafana dashboard: its title, the exporter keys
// its tags name, and every non-empty query of its panels (the panels of
// collapsed rows included), annotations and query variables. CheckDashboard
// and the Compose smoke test (tools/obssmoke) read dashboards through it.
func DashboardQueries(raw []byte) (Dashboard, error) {
	var d dashboardQueries
	if err := json.Unmarshal(raw, &d); err != nil {
		return Dashboard{}, fmt.Errorf("obsgen: parse dashboard: %w", err)
	}
	out := Dashboard{Title: d.Title, Exporters: d.exporters()}
	add := func(where, expr, ds string) {
		if expr != "" {
			out.Queries = append(out.Queries, Query{Where: where, Expr: expr, Datasource: ds})
		}
	}
	for _, p := range flattenPanels(d.Panels) {
		for _, t := range p.Targets {
			add("panel "+p.Title, t.Expr, t.Datasource.Type)
		}
	}
	for _, a := range d.Annotations.List {
		add("annotation "+a.Name, a.Expr, "")
	}
	for _, v := range d.Templating.List {
		add("variable "+v.Name, variableExpr(v.Query), v.Datasource.Type)
	}
	return out, nil
}

// CheckDashboard returns one problem per Prometheus query of a Grafana
// dashboard (panels, nested row panels, annotations and template variables)
// that names a series nothing emits.
func CheckDashboard(raw []byte) ([]string, error) {
	d, err := DashboardQueries(raw)
	if err != nil {
		return nil, err
	}
	var problems []string
	for _, q := range d.Queries {
		// Only Prometheus queries name metric series; a Loki query is LogQL.
		if q.Datasource == "loki" {
			continue
		}
		for _, name := range CheckExpr(q.Expr, d.Exporters...) {
			problems = append(problems, fmt.Sprintf("%s: %q queries %s, which nothing emits", d.Title, q.Where, name))
		}
		for _, p := range CheckBucketMatchers(q.Expr) {
			problems = append(problems, fmt.Sprintf("%s: %q: %s", d.Title, q.Where, p))
		}
	}
	return problems, nil
}

// CheckRules returns one problem per rule of a Prometheus rule file whose
// expression names a series nothing emits.
func CheckRules(raw []byte) ([]string, error) {
	var f struct {
		Groups []struct {
			Name  string `yaml:"name"`
			Rules []struct {
				Record string `yaml:"record"`
				Alert  string `yaml:"alert"`
				Expr   string `yaml:"expr"`
			} `yaml:"rules"`
		} `yaml:"groups"`
	}
	if err := yaml.Unmarshal(raw, &f); err != nil {
		return nil, fmt.Errorf("obsgen: parse rule file: %w", err)
	}
	var problems []string
	for _, g := range f.Groups {
		for _, r := range g.Rules {
			for _, name := range CheckExpr(r.Expr) {
				problems = append(problems, fmt.Sprintf("%s: rule %s%s queries %s, which nothing emits", g.Name, r.Record, r.Alert, name))
			}
			for _, p := range CheckBucketMatchers(r.Expr) {
				problems = append(problems, fmt.Sprintf("%s: rule %s%s: %s", g.Name, r.Record, r.Alert, p))
			}
		}
	}
	return problems, nil
}

// dashboardQueries is the part of a dashboard's JSON that holds queries.
type dashboardQueries struct {
	Title       string       `json:"title"`
	Tags        []string     `json:"tags"`
	Panels      []panelQuery `json:"panels"`
	Annotations struct {
		List []struct {
			Name string `json:"name"`
			Expr string `json:"expr"`
		} `json:"list"`
	} `json:"annotations"`
	Templating struct {
		List []struct {
			Name       string          `json:"name"`
			Query      json.RawMessage `json:"query"`
			Datasource struct {
				Type string `json:"type"`
			} `json:"datasource"`
		} `json:"list"`
	} `json:"templating"`
}

// exporters returns the exporter keys the dashboard's tags name.
func (d dashboardQueries) exporters() []string {
	var out []string
	for _, t := range d.Tags {
		if key, ok := strings.CutPrefix(t, exporterTagPrefix); ok {
			out = append(out, key)
		}
	}
	return out
}

type panelQuery struct {
	Title   string `json:"title"`
	Targets []struct {
		Expr       string `json:"expr"`
		Datasource struct {
			Type string `json:"type"`
		} `json:"datasource"`
	} `json:"targets"`
	Panels []panelQuery `json:"panels"`
}

// flattenPanels returns the panels and the panels of collapsed rows, which
// Grafana nests one level deep.
func flattenPanels(panels []panelQuery) []panelQuery {
	out := slices.Clone(panels)
	for _, p := range panels {
		out = append(out, p.Panels...)
	}
	return out
}

var labelValuesRe = regexp.MustCompile(`^\s*(?:label_values|query_result)\((.*?)(?:,\s*[a-zA-Z_][a-zA-Z0-9_]*)?\)\s*$`)

// variableExpr returns the PromQL inside a query variable's
// label_values(expr, label) or query_result(expr), or "" for any other
// variable (a datasource, a custom list).
func variableExpr(raw json.RawMessage) string {
	var q string
	if json.Unmarshal(raw, &q) != nil {
		var m struct {
			Query string `json:"query"`
		}
		if json.Unmarshal(raw, &m) != nil {
			return ""
		}
		q = m.Query
	}
	if m := labelValuesRe.FindStringSubmatch(q); m != nil {
		return m[1]
	}
	return ""
}

// promqlWords are the PromQL words that are neither functions (always
// followed by "(") nor metric names.
var promqlWords = map[string]bool{
	"by": true, "without": true, "on": true, "ignoring": true, "group_left": true, "group_right": true,
	"and": true, "or": true, "unless": true, "bool": true, "offset": true, "atan2": true,
	"inf": true, "nan": true, "Inf": true, "NaN": true,
	"sum": true, "min": true, "max": true, "avg": true, "group": true, "stddev": true, "stdvar": true,
	"count": true, "count_values": true, "bottomk": true, "topk": true, "quantile": true,
	"limitk": true, "limit_ratio": true,
}

// MetricNames returns the metric names a PromQL expression selects, in order
// of first appearance. It reads PromQL's lexical structure: strings, label
// matchers in {}, ranges in [], Grafana variables ($x, ${x}), numbers and
// durations are skipped; a name followed by "(" is a function; the grouping
// lists after by, without, on, ignoring, group_left and group_right are
// label names.
func MetricNames(expr string) []string {
	var names []string
	lx := lexer{s: expr}
	for lx.i < len(lx.s) {
		name := lx.next()
		if name != "" && !slices.Contains(names, name) {
			names = append(names, name)
		}
	}
	return names
}

// lexer walks a PromQL expression; next advances past one token and returns
// it when it is a metric name.
type lexer struct {
	s string
	i int
}

func (lx *lexer) next() string {
	c := lx.s[lx.i]
	switch {
	case c == '"' || c == '\'' || c == '`':
		lx.skipString(c)
	case c == '{':
		lx.skipTo('}')
	case c == '[':
		lx.skipTo(']')
	case c == '$':
		lx.skipVariable()
	case isDigit(c) || (c == '.' && lx.i+1 < len(lx.s) && isDigit(lx.s[lx.i+1])):
		lx.skipWord()
	case isNameStart(c):
		return lx.name()
	default:
		lx.i++
	}
	return ""
}

// name reads an identifier and classifies it.
func (lx *lexer) name() string {
	start := lx.i
	lx.skipWord()
	word := lx.s[start:lx.i]
	followedByParen := lx.peekNonSpace() == '('
	switch {
	case isGrouping(word) && followedByParen:
		lx.skipTo(')') // a label list, not a call
		return ""
	case followedByParen, promqlWords[word]:
		return ""
	default:
		return word
	}
}

func isGrouping(w string) bool {
	switch w {
	case "by", "without", "on", "ignoring", "group_left", "group_right":
		return true
	}
	return false
}

// peekNonSpace returns the next non-space byte without consuming it.
func (lx *lexer) peekNonSpace() byte {
	for j := lx.i; j < len(lx.s); j++ {
		if !isSpace(lx.s[j]) {
			return lx.s[j]
		}
	}
	return 0
}

// skipTo advances past the next unquoted close byte (from the current
// position, which may be the opening byte).
func (lx *lexer) skipTo(close byte) {
	lx.i++
	for lx.i < len(lx.s) {
		c := lx.s[lx.i]
		switch {
		case c == '"' || c == '\'' || c == '`':
			lx.skipString(c)
		case c == close:
			lx.i++
			return
		default:
			lx.i++
		}
	}
}

// skipString advances past a string literal opened by quote.
func (lx *lexer) skipString(quote byte) {
	lx.i++
	for lx.i < len(lx.s) {
		c := lx.s[lx.i]
		lx.i++
		if c == '\\' && quote != '`' {
			lx.i++
			continue
		}
		if c == quote {
			return
		}
	}
}

// skipVariable advances past a Grafana variable: $name or ${name}.
func (lx *lexer) skipVariable() {
	lx.i++
	if lx.i < len(lx.s) && lx.s[lx.i] == '{' {
		lx.skipTo('}')
		return
	}
	lx.skipWord()
}

// skipWord advances past a run of name characters (identifiers, numbers,
// durations).
func (lx *lexer) skipWord() {
	for lx.i < len(lx.s) && (isNameStart(lx.s[lx.i]) || isDigit(lx.s[lx.i]) || lx.s[lx.i] == '.') {
		lx.i++
	}
}

func isDigit(c byte) bool     { return c >= '0' && c <= '9' }
func isSpace(c byte) bool     { return strings.IndexByte(" \t\r\n", c) >= 0 }
func isNameStart(c byte) bool { return c == '_' || c == ':' || (c|0x20 >= 'a' && c|0x20 <= 'z') }

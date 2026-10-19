// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

// Package obsgen generates the observability files from the metric
// definition (pkg/observability/metricdef): the Grafana dashboards, built with
// the Grafana Foundation SDK, and the metric reference page. It also holds the
// contract check that refuses a dashboard query naming a series nothing
// emits. tools/obsgen writes the files; TestGeneratedFilesAreCurrent fails
// when a committed copy differs from what this package generates.
package obsgen

import (
	"bytes"
	"encoding/json"
	"fmt"
	"strings"

	"github.com/grafana/grafana-foundation-sdk/go/dashboard"
)

// File is one generated file: its path from the repository root and its
// content. When Region is set, Content is only the block of the file between
// the lines "# BEGIN <Region>" and "# END <Region>"; the rest of the file is
// written by hand (Merge).
type File struct {
	Path    string
	Content []byte
	Region  string
}

// Merge returns the file f writes over old: Content, or for a region old with
// the lines between its markers replaced by Content, indented as the BEGIN
// marker is.
func (f File) Merge(old []byte) ([]byte, error) {
	if f.Region == "" {
		return f.Content, nil
	}
	lines := strings.SplitAfter(string(old), "\n")
	begin, end := -1, -1
	for i, l := range lines {
		switch strings.TrimSpace(l) {
		case "# BEGIN " + f.Region:
			begin = i
		case "# END " + f.Region:
			end = i
		}
	}
	if begin < 0 || end < begin {
		return nil, fmt.Errorf("obsgen: %s has no \"# BEGIN %s\" ... \"# END %s\" block", f.Path, f.Region, f.Region)
	}
	indent := lines[begin][:len(lines[begin])-len(strings.TrimLeft(lines[begin], " "))]
	var b strings.Builder
	for _, l := range lines[:begin+1] {
		b.WriteString(l)
	}
	for _, l := range strings.SplitAfter(string(f.Content), "\n") {
		if l != "" {
			b.WriteString(indent + l)
		}
	}
	for _, l := range lines[end:] {
		b.WriteString(l)
	}
	return []byte(b.String()), nil
}

// DashboardDir holds the generated dashboards, one JSON file each.
const DashboardDir = "deploy/grafana/dashboards"

// MetricsReference is the generated metric reference page.
const MetricsReference = "docs/observability/metrics.md"

// generatedDashboard is one dashboard and the file it is written to.
type generatedDashboard struct {
	file    string
	builder *dashboard.DashboardBuilder
}

// dashboards lists every generated dashboard: the VMAFx dashboards, then one
// per vendor GPU exporter.
func dashboards() []generatedDashboard {
	out := []generatedDashboard{
		{"vmafx-overview.json", overview()},
		{"vmafx-quality.json", quality()},
		{"vmafx-nodes.json", nodes()},
		{"vmafx-live.json", live()},
		{"vmafx-slo.json", sloDashboard()},
		{"vmafx-usage.json", usage()},
		{"vmafx-capacity.json", capacity()},
	}
	for _, e := range gpuExporters() {
		out = append(out, generatedDashboard{"vmafx-gpu-" + e.key + ".json", exporterDashboard(e)})
	}
	return out
}

// Generate returns every generated file, in a stable order.
func Generate() ([]File, error) {
	var out []File
	for _, d := range dashboards() {
		content, err := renderDashboard(d.builder)
		if err != nil {
			return nil, fmt.Errorf("obsgen: %s: %w", d.file, err)
		}
		out = append(out,
			File{Path: DashboardDir + "/" + d.file, Content: content},
			File{Path: HelmDashboardDir + "/" + d.file, Content: content})
	}
	rules, err := RenderRules(DefaultSettings())
	if err != nil {
		return nil, err
	}
	tests, err := rulesTestYAML()
	if err != nil {
		return nil, err
	}
	chartRules, err := helmRuleTemplate()
	if err != nil {
		return nil, err
	}
	datasources, err := datasourcesYAML()
	if err != nil {
		return nil, err
	}
	return append(out,
		File{Path: DatasourcesFile, Content: datasources},
		File{Path: RulesFile, Content: rules},
		File{Path: RulesTestFile, Content: tests},
		File{Path: HelmRuleTemplate, Content: chartRules},
		File{Path: HelmValuesFile, Content: settingsValues(DefaultSettings()), Region: SettingsRegion},
		File{Path: MetricsReference, Content: metricsReference()},
	), nil
}

// renderDashboard builds b and returns its JSON, indented, with a final
// newline.
func renderDashboard(b *dashboard.DashboardBuilder) ([]byte, error) {
	d, err := b.Build()
	if err != nil {
		return nil, err
	}
	raw, err := json.Marshal(d)
	if err != nil {
		return nil, err
	}
	var buf bytes.Buffer
	if err := json.Indent(&buf, raw, "", "  "); err != nil {
		return nil, err
	}
	buf.WriteByte('\n')
	return buf.Bytes(), nil
}

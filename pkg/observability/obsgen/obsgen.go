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

	"github.com/grafana/grafana-foundation-sdk/go/dashboard"
)

// File is one generated file: its path from the repository root and its
// content.
type File struct {
	Path    string
	Content []byte
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
		{"vmafx-logs.json", logsDashboard()},
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
		out = append(out, File{Path: DashboardDir + "/" + d.file, Content: content})
	}
	rules, err := rulesYAML()
	if err != nil {
		return nil, err
	}
	tests, err := rulesTestYAML()
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

// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package scoringservice

import (
	"testing"

	"github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

func TestProvideMetricsRegistersRuntimeAndServiceCollectors(t *testing.T) {
	t.Parallel()

	registry, metrics, err := ProvideMetrics()
	if err != nil {
		t.Fatalf("ProvideMetrics: %v", err)
	}
	if registry == nil || metrics == nil {
		t.Fatal("ProvideMetrics returned a nil dependency")
	}
	families, err := registry.Gather()
	if err != nil {
		t.Fatalf("gather metrics: %v", err)
	}
	seen := make(map[string]bool, len(families))
	for _, family := range families {
		seen[family.GetName()] = true
	}
	for _, name := range []string{
		"go_goroutines",
		"process_cpu_seconds_total",
		"vmafx_server_score_requests_total",
	} {
		if !seen[name] {
			t.Errorf("metric family %q is not registered", name)
		}
	}
}

// TestProvideMetricsServesTheServerFamilies is the contract between metricdef
// and vmafx-server, whose /metrics page is this registry: after one scored
// request every family metricdef lists for the server is served, and no
// vmafx_ family is served that metricdef does not list for it.
func TestProvideMetricsServesTheServerFamilies(t *testing.T) {
	t.Parallel()
	registry, metrics, err := ProvideMetrics()
	if err != nil {
		t.Fatalf("ProvideMetrics: %v", err)
	}
	metrics.ScoreDuration.Observe(1)
	metrics.ObserveScore("", "", 93)
	families, err := registry.Gather()
	if err != nil {
		t.Fatalf("gather metrics: %v", err)
	}
	served := map[string]bool{}
	for _, f := range families {
		served[f.GetName()] = true
	}
	defined := map[string]bool{}
	for _, f := range metricdef.ByEmitter(metricdef.Server) {
		defined[f.Name] = true
		if !served[f.Name] {
			t.Errorf("metricdef lists %s for the server, /metrics does not serve it", f.Name)
		}
	}
	for name := range served {
		if metricdef.IsVMAFx(name) && !defined[name] {
			t.Errorf("/metrics serves %s, which metricdef does not list for the server", name)
		}
	}
}

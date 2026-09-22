// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package scoringservice

import "testing"

func TestProvideMetricsRegistersRuntimeAndServiceCollectors(t *testing.T) {
	t.Parallel()

	registry, metrics := ProvideMetrics()
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

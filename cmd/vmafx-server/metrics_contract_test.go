// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package main

import (
	"testing"

	"github.com/VMAFx/vmafx/internal/app/scoringservice"
	"github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

// TestServerServesEveryFamilyItEmits is the contract between metricdef and
// vmafx-server: its /metrics registry, built by the production providers
// (ProvideMetrics, provideStreamMetrics), serves after one scored request and
// one stream session every family metricdef lists for the server, and no
// vmafx_ family metricdef does not list for it.
func TestServerServesEveryFamilyItEmits(t *testing.T) {
	t.Parallel()
	registry, metrics, err := scoringservice.ProvideMetrics()
	if err != nil {
		t.Fatal(err)
	}
	streams, err := provideStreamMetrics(registry)
	if err != nil {
		t.Fatal(err)
	}
	metrics.ScoreDuration.Observe(1)
	metrics.ObserveScore("", "", 93)
	session := streams.Begin()
	session.Frame()
	session.End(nil)
	families, err := registry.Gather()
	if err != nil {
		t.Fatal(err)
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

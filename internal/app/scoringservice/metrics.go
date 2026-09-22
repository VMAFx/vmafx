// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

// Package scoringservice owns application-level wiring shared by the VMAFx
// scoring server and controller.
package scoringservice

import (
	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/collectors"

	"github.com/VMAFx/vmafx/pkg/observability"
)

// ProvideMetrics builds an isolated Prometheus registry and the VMAFx metric
// instruments used by both scoring services.
func ProvideMetrics() (*prometheus.Registry, *observability.Metrics) {
	registry := prometheus.NewRegistry()
	registry.MustRegister(collectors.NewGoCollector())
	registry.MustRegister(collectors.NewProcessCollector(collectors.ProcessCollectorOpts{}))
	return registry, observability.NewMetrics(registry)
}

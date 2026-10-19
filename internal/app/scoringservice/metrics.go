// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

// Package scoringservice owns application-level wiring shared by the VMAFx
// scoring server and controller.
package scoringservice

import (
	"github.com/prometheus/client_golang/prometheus"

	"github.com/VMAFx/vmafx/pkg/observability"
)

// ProvideMetrics builds an isolated Prometheus registry and the VMAFx metric
// instruments used by both scoring services.
func ProvideMetrics() (*prometheus.Registry, *observability.Metrics, error) {
	registry, err := observability.NewRegistry()
	if err != nil {
		return nil, nil, err
	}
	metrics, err := observability.NewMetrics(registry)
	if err != nil {
		return nil, nil, err
	}
	return registry, metrics, nil
}

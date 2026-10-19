// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package oteltest

import (
	"testing"

	"github.com/prometheus/client_golang/prometheus"

	"github.com/VMAFx/vmafx/pkg/observability"
)

// Metrics registers the scoring and quality families on reg and fails the
// test when that fails, so a handler test gets its *observability.Metrics in
// one line.
func Metrics(t testing.TB, reg prometheus.Registerer) *observability.Metrics {
	t.Helper()
	m, err := observability.NewMetrics(reg)
	if err != nil {
		t.Fatalf("observability.NewMetrics: %v", err)
	}
	return m
}

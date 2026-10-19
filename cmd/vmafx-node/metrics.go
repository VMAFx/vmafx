// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

//go:build cgo

package main

import (
	"context"
	"errors"
	"time"

	"github.com/prometheus/client_golang/prometheus"

	"github.com/VMAFx/vmafx/internal/app/scoringservice"
	"github.com/VMAFx/vmafx/pkg/observability"
	"github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

// nodeMetrics are the node's families (metricdef): what it runs on, its
// slots, and the controller jobs it runs.
type nodeMetrics struct {
	slots    observability.Gauge
	running  observability.Gauge
	jobs     observability.Counter
	duration observability.Histogram
}

// provideNodeMetrics registers the node's families on reg and records the
// backend the executor runs on; device memory is read for that backend.
func provideNodeMetrics(reg *prometheus.Registry, exec *Executor) (*nodeMetrics, error) {
	return newNodeMetrics(reg, exec.backend, deviceMemoryFor(exec.backend))
}

// newNodeMetrics registers the node's families on reg. read is the device
// memory source of the backend, nil when the backend has none: the families
// are registered and serve no series.
func newNodeMetrics(reg *prometheus.Registry, backend string, read deviceMemoryReader) (*nodeMetrics, error) {
	if err := registerDeviceMemory(reg, read); err != nil {
		return nil, err
	}
	info, err := observability.NewGauge(reg, metricdef.NodeInfo)
	if err != nil {
		return nil, err
	}
	m := &nodeMetrics{}
	gauges := map[*observability.Gauge]metricdef.Family{&m.slots: metricdef.NodeSlots, &m.running: metricdef.NodeJobsRunning}
	for dst, f := range gauges {
		if *dst, err = observability.NewGauge(reg, f); err != nil {
			return nil, err
		}
	}
	if m.jobs, err = observability.NewCounter(reg, metricdef.NodeJobs); err != nil {
		return nil, err
	}
	if m.duration, err = observability.NewHistogram(reg, metricdef.NodeJobDuration); err != nil {
		return nil, err
	}
	info.Set(1, backend, backendVendors[backend])
	m.slots.Set(0)
	m.running.Set(0)
	return m, nil
}

// registerDeviceMemory registers the scraped device memory families; a
// failed read counts in vmafx_metrics_read_errors_total{source="device_memory"}.
func registerDeviceMemory(reg *prometheus.Registry, read deviceMemoryReader) error {
	errs, err := observability.NewReadErrors(reg)
	if err != nil {
		return err
	}
	if read == nil {
		read = func(context.Context) ([]deviceMemory, error) { return nil, nil }
	}
	return observability.RegisterScraped(reg, errs, observability.ScrapeGroup{
		Source:   "device_memory",
		Families: []metricdef.Family{metricdef.NodeDeviceMemoryUsed, metricdef.NodeDeviceMemoryTotal},
		Read:     deviceMemoryScrape(read),
	})
}

// provideStreamMetrics registers the ScoreStream session families on the
// node's registry.
func provideStreamMetrics(reg *prometheus.Registry) (*scoringservice.StreamMetrics, error) {
	return scoringservice.NewStreamMetrics(reg)
}

// provideNodeRegistry is the registry the node serves on /metrics.
func provideNodeRegistry() (*prometheus.Registry, error) {
	return observability.NewRegistry()
}

// setSlots records the slot count the controller client runs with. A nil
// receiver (tests without metrics) records nothing.
func (m *nodeMetrics) setSlots(n int) {
	if m != nil {
		m.slots.Set(float64(n))
	}
}

// jobStarted counts a job the node began running.
func (m *nodeMetrics) jobStarted() {
	if m != nil {
		m.running.Add(1)
	}
}

// jobDone records a job that ended: the running count, its outcome on its
// backend and how long it ran.
func (m *nodeMetrics) jobDone(backend string, err error, elapsed time.Duration) {
	if m == nil {
		return
	}
	m.running.Add(-1)
	m.jobs.Inc(backend, jobOutcome(err))
	m.duration.Observe(elapsed.Seconds(), backend)
}

// jobOutcome is the outcome label of a job that ended with err.
func jobOutcome(err error) string {
	switch {
	case err == nil:
		return "completed"
	case errors.Is(err, errCancelledByController):
		return "cancelled"
	default:
		return "failed"
	}
}

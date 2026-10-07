// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package main

import (
	"context"
	"fmt"
	"time"

	"github.com/prometheus/client_golang/prometheus"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/nodes"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/queue"
	"github.com/VMAFx/vmafx/pkg/observability"
	"github.com/VMAFx/vmafx/pkg/observability/metricdef"
	"github.com/VMAFx/vmafx/pkg/registry"
)

// controllerMetrics are the controller's job families (metricdef) and the
// shared scoring metrics whose quality family a completed job feeds. The queue
// and node-registry families are read at scrape time (registerQueueCollector).
type controllerMetrics struct {
	scoring   *observability.Metrics
	submitted observability.Counter
	completed observability.Counter
	failed    observability.Counter
	cancelled observability.Counter
	queueWait observability.Histogram
	duration  observability.Histogram
}

// newControllerMetrics registers the controller's event-driven job families
// on reg, the registry /metrics serves.
func newControllerMetrics(reg *prometheus.Registry, scoring *observability.Metrics) (*controllerMetrics, error) {
	m := &controllerMetrics{scoring: scoring}
	counters := map[*observability.Counter]metricdef.Family{
		&m.submitted: metricdef.ControllerJobsSubmitted,
		&m.completed: metricdef.ControllerJobsCompleted,
		&m.failed:    metricdef.ControllerJobsFailed,
		&m.cancelled: metricdef.ControllerJobsCancelled,
	}
	for dst, f := range counters {
		c, err := observability.NewCounter(reg, f)
		if err != nil {
			return nil, err
		}
		*dst = c
	}
	var err error
	if m.queueWait, err = observability.NewHistogram(reg, metricdef.ControllerJobQueueWait); err != nil {
		return nil, err
	}
	if m.duration, err = observability.NewHistogram(reg, metricdef.ControllerJobDuration); err != nil {
		return nil, err
	}
	return m, nil
}

// jobSubmitted counts a job the queue accepted.
func (m *controllerMetrics) jobSubmitted(tenant string) {
	m.submitted.Inc(tenant)
}

// jobAssigned records how long job waited in the queue before PullWork handed
// it to a node.
func (m *controllerMetrics) jobAssigned(job *queue.Job) {
	if !job.CreatedAt.IsZero() {
		m.queueWait.Observe(sinceSeconds(job.CreatedAt), job.TenantID)
	}
}

// jobFinished counts a job this controller moved to a terminal state, records
// its time from submission and, for a completed job, its score in the quality
// family. status is queue.StatusCompleted, StatusFailed or StatusCancelled. A
// job without a submission time (it could not be read back) is counted only.
func (m *controllerMetrics) jobFinished(job *queue.Job, status string) {
	switch status {
	case queue.StatusCompleted:
		m.completed.Inc(job.TenantID)
	case queue.StatusFailed:
		m.failed.Inc(job.TenantID)
	case queue.StatusCancelled:
		m.cancelled.Inc(job.TenantID)
	default:
		return
	}
	if job.CreatedAt.IsZero() {
		return
	}
	m.duration.Observe(sinceSeconds(job.CreatedAt), job.TenantID, status)
	if status == queue.StatusCompleted {
		m.scoring.ObserveScore(job.TenantID, job.Scoring.Model, job.Score)
	}
}

// sinceSeconds is the time since t in seconds, never negative (the queue
// stores whole seconds, so a fresh job can read as slightly in the future).
func sinceSeconds(t time.Time) float64 {
	return max(time.Since(t).Seconds(), 0)
}

// queueStats is the part of the queue the scrape reads.
type queueStats interface {
	Stats(ctx context.Context) (queue.Stats, error)
}

// registerQueueCollector registers the scraped families: per-tenant pending
// and running jobs, the age of each tenant's oldest pending job, the requeue
// totals and the live node count, read from q and r when Prometheus scrapes.
// A failed read counts in vmafx_metrics_read_errors_total{source="queue"}.
func registerQueueCollector(reg *prometheus.Registry, q queue.Queue, r *nodes.Registry) error {
	errs, err := observability.NewReadErrors(reg)
	if err != nil {
		return fmt.Errorf("controller metrics: %w", err)
	}
	group := observability.ScrapeGroup{
		Source: "queue",
		Families: []metricdef.Family{
			metricdef.ControllerJobsPending, metricdef.ControllerJobsRunning,
			metricdef.ControllerQueueOldestAge, metricdef.ControllerJobsRequeued,
			metricdef.ControllerNodesLive,
		},
		Read: queueScrape(q, r),
	}
	if err := observability.RegisterScraped(reg, errs, group); err != nil {
		return fmt.Errorf("controller metrics: %w", err)
	}
	return nil
}

// queueScrape reads the queue's counts and the node registry's size.
func queueScrape(q queueStats, r registry.Counter) observability.ScrapeFunc {
	return func(ctx context.Context) ([]observability.Sample, error) {
		st, err := q.Stats(ctx)
		if err != nil {
			return nil, err
		}
		samples := tenantSamples(st.Tenants, time.Now())
		for _, reason := range metricdef.RequeueReason.Values {
			samples = append(samples, observability.Sample{
				Family: metricdef.ControllerJobsRequeued, Value: float64(st.Requeued[reason]), Labels: []string{reason},
			})
		}
		return append(samples, observability.Sample{
			Family: metricdef.ControllerNodesLive, Value: float64(r.Count()),
		}), nil
	}
}

// tenantSamples turns the queue's per-tenant counts into samples. A tenant
// with no pending job has no oldest-age sample.
func tenantSamples(tenants []queue.TenantStats, now time.Time) []observability.Sample {
	out := make([]observability.Sample, 0, 3*len(tenants))
	for _, ts := range tenants {
		lv := []string{ts.TenantID}
		out = append(out,
			observability.Sample{Family: metricdef.ControllerJobsPending, Value: float64(ts.Pending), Labels: lv},
			observability.Sample{Family: metricdef.ControllerJobsRunning, Value: float64(ts.Running), Labels: lv})
		if !ts.OldestPending.IsZero() {
			age := max(now.Sub(ts.OldestPending).Seconds(), 0)
			out = append(out, observability.Sample{Family: metricdef.ControllerQueueOldestAge, Value: age, Labels: lv})
		}
	}
	return out
}

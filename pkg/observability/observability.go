// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/observability/observability.go — structured logging, Prometheus metrics,
// and graceful-shutdown helpers for the vmafx-controller.
//
// Logging: Go 1.21 stdlib log/slog, JSON handler, emitted to stdout.
// Metrics: github.com/prometheus/client_golang — per-request counters and
//          latency histogram for both gRPC and HTTP transports, plus
//          controller-specific gauges and counters (Phase 4b.1, ADR-0711).
//
// ADR-0703: vmafx-server Go gRPC + HTTP service (origin).
// ADR-0711: vmafx-controller Phase 4b.1 scope expansion.

package observability

import (
	"context"
	"log/slog"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promauto"
)

// ControllerStatSource is satisfied by types that can report controller gauge
// values (job counts, node counts).  Defined here to avoid a circular import
// between pkg/observability and cmd/vmafx-controller/queue.
type ControllerStatSource interface {
	PendingCount() int
	RunningCount() int
}

// NodeStatSource is satisfied by the node registry.
type NodeStatSource interface {
	Count() int
}

// GracefulShutdownTimeout is the maximum time the server waits for in-flight
// requests to drain after receiving SIGTERM / SIGINT.
const GracefulShutdownTimeout = 30 * time.Second

// NewLogger creates a JSON-structured slog.Logger writing to stdout.
// levelStr is a slog.Level string (e.g. "DEBUG", "INFO", "WARN", "ERROR").
// Unrecognised strings default to INFO.
func NewLogger(levelStr string) *slog.Logger {
	var level slog.Level
	if err := level.UnmarshalText([]byte(levelStr)); err != nil {
		level = slog.LevelInfo
	}
	opts := &slog.HandlerOptions{Level: level}
	handler := slog.NewJSONHandler(os.Stdout, opts)
	return slog.New(handler)
}

// Metrics holds all Prometheus instruments registered by the vmafx-controller.
type Metrics struct {
	// ScoreRequests is the total number of /v1/score / Score RPC calls.
	ScoreRequests prometheus.Counter
	// ScoreErrors is the total number of scoring errors.
	ScoreErrors prometheus.Counter
	// ScoreDuration tracks scoring latency in seconds.
	ScoreDuration prometheus.Histogram
	// HealthRequests counts /healthz + Health RPC calls.
	HealthRequests prometheus.Counter
	// ReadyRequests counts /readyz calls.
	ReadyRequests prometheus.Counter

	// Controller metrics (Phase 4b.1 — ADR-0711).

	// JobsPending is the current number of PENDING jobs.
	JobsPending prometheus.Gauge
	// JobsRunning is the current number of RUNNING jobs.
	JobsRunning prometheus.Gauge
	// NodesRegistered is the current number of live vmafx-node registrations.
	NodesRegistered prometheus.Gauge
	// JobsSubmitted is the total number of jobs submitted via SubmitJob RPC.
	JobsSubmitted prometheus.Counter
	// JobsCompleted is the total number of jobs completed successfully.
	JobsCompleted prometheus.Counter
	// JobsFailed is the total number of jobs that ended in failure.
	JobsFailed prometheus.Counter
	// JobsCancelled is the total number of jobs cancelled.
	JobsCancelled prometheus.Counter

	// internal: sources for gauge updates.
	queueSource ControllerStatSource
	nodeSource  NodeStatSource
}

// NewMetrics registers and returns the vmafx-controller Prometheus metrics.
// reg must be a *prometheus.Registry (use prometheus.NewRegistry() for tests
// or prometheus.DefaultRegisterer for production).
func NewMetrics(reg prometheus.Registerer) *Metrics {
	factory := promauto.With(reg)
	return &Metrics{
		// Phase 4a scoring metrics.
		ScoreRequests: factory.NewCounter(prometheus.CounterOpts{
			Namespace: "vmafx",
			Subsystem: "controller",
			Name:      "score_requests_total",
			Help:      "Total number of direct Score requests (HTTP + gRPC VmafxScoring).",
		}),
		ScoreErrors: factory.NewCounter(prometheus.CounterOpts{
			Namespace: "vmafx",
			Subsystem: "controller",
			Name:      "score_errors_total",
			Help:      "Total number of direct Score requests that returned an error.",
		}),
		ScoreDuration: factory.NewHistogram(prometheus.HistogramOpts{
			Namespace: "vmafx",
			Subsystem: "controller",
			Name:      "score_duration_seconds",
			Help:      "End-to-end duration of a direct Score request in seconds.",
			Buckets:   prometheus.DefBuckets,
		}),
		HealthRequests: factory.NewCounter(prometheus.CounterOpts{
			Namespace: "vmafx",
			Subsystem: "controller",
			Name:      "health_requests_total",
			Help:      "Total number of Health / healthz requests.",
		}),
		ReadyRequests: factory.NewCounter(prometheus.CounterOpts{
			Namespace: "vmafx",
			Subsystem: "controller",
			Name:      "ready_requests_total",
			Help:      "Total number of readyz requests.",
		}),

		// Phase 4b.1 controller metrics (ADR-0711).
		JobsPending: factory.NewGauge(prometheus.GaugeOpts{
			Namespace: "vmafx",
			Subsystem: "controller",
			Name:      "jobs_pending",
			Help:      "Current number of PENDING jobs in the queue.",
		}),
		JobsRunning: factory.NewGauge(prometheus.GaugeOpts{
			Namespace: "vmafx",
			Subsystem: "controller",
			Name:      "jobs_running",
			Help:      "Current number of RUNNING jobs assigned to nodes.",
		}),
		NodesRegistered: factory.NewGauge(prometheus.GaugeOpts{
			Namespace: "vmafx",
			Subsystem: "controller",
			Name:      "nodes_registered",
			Help:      "Current number of live vmafx-node registrations.",
		}),
		JobsSubmitted: factory.NewCounter(prometheus.CounterOpts{
			Namespace: "vmafx",
			Subsystem: "controller",
			Name:      "jobs_submitted_total",
			Help:      "Total number of jobs submitted via the SubmitJob RPC.",
		}),
		JobsCompleted: factory.NewCounter(prometheus.CounterOpts{
			Namespace: "vmafx",
			Subsystem: "controller",
			Name:      "jobs_completed_total",
			Help:      "Total number of jobs that completed successfully.",
		}),
		JobsFailed: factory.NewCounter(prometheus.CounterOpts{
			Namespace: "vmafx",
			Subsystem: "controller",
			Name:      "jobs_failed_total",
			Help:      "Total number of jobs that ended in failure.",
		}),
		JobsCancelled: factory.NewCounter(prometheus.CounterOpts{
			Namespace: "vmafx",
			Subsystem: "controller",
			Name:      "jobs_cancelled_total",
			Help:      "Total number of cancelled jobs.",
		}),
	}
}

// SetControllerSources wires the gauge sources for job and node counts.
// Must be called after queue and registry are initialised.
// Starts a background goroutine that refreshes the gauges every 15 s.
func (m *Metrics) SetControllerSources(q ControllerStatSource, r NodeStatSource) {
	m.queueSource = q
	m.nodeSource = r
	go m.gaugeRefreshLoop()
}

// gaugeRefreshLoop refreshes the controller gauges on a fixed interval.
func (m *Metrics) gaugeRefreshLoop() {
	ticker := time.NewTicker(15 * time.Second)
	defer ticker.Stop()
	for range ticker.C {
		if m.queueSource != nil {
			m.JobsPending.Set(float64(m.queueSource.PendingCount()))
			m.JobsRunning.Set(float64(m.queueSource.RunningCount()))
		}
		if m.nodeSource != nil {
			m.NodesRegistered.Set(float64(m.nodeSource.Count()))
		}
	}
}

// WaitForShutdown blocks until SIGTERM or SIGINT is received, then cancels
// the context returned by NewShutdownContext and waits up to timeout for the
// caller to drain in-flight requests.
//
// Typical usage:
//
//	ctx, stop := observability.NewShutdownContext()
//	defer stop()
//	// ... start servers using ctx ...
//	observability.WaitForShutdown(ctx, log, observability.GracefulShutdownTimeout)
func WaitForShutdown(ctx context.Context, log *slog.Logger, timeout time.Duration) {
	ch := make(chan os.Signal, 1)
	signal.Notify(ch, syscall.SIGTERM, syscall.SIGINT)
	defer signal.Stop(ch)

	select {
	case sig := <-ch:
		log.Info("shutdown signal received", "signal", sig.String())
	case <-ctx.Done():
		log.Info("context cancelled; initiating shutdown")
	}

	// Allow callers timeout to finish gracefully.
	deadline := time.After(timeout)
	<-deadline
}

// NewShutdownContext returns a context that is cancelled on SIGTERM / SIGINT.
// The returned stop function must be called when the context is no longer needed
// to release resources.
func NewShutdownContext() (context.Context, context.CancelFunc) {
	ctx, cancel := context.WithCancel(context.Background())
	go func() {
		ch := make(chan os.Signal, 1)
		signal.Notify(ch, syscall.SIGTERM, syscall.SIGINT)
		defer signal.Stop(ch)
		<-ch
		cancel()
	}()
	return ctx, cancel
}

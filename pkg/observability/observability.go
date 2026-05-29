// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/observability/observability.go — structured logging, Prometheus metrics,
// and graceful-shutdown helpers for the vmafx-server.
//
// Logging: Go 1.21 stdlib log/slog, JSON handler, emitted to stdout.
// Metrics: github.com/prometheus/client_golang — per-request counters and
//          latency histogram for both gRPC and HTTP transports.
//
// ADR-0703: vmafx-server Go gRPC + HTTP service.

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

// GracefulShutdownTimeout is the maximum time the server waits for in-flight
// requests to drain after receiving SIGTERM / SIGINT.
const GracefulShutdownTimeout = 30 * time.Second

// jobQueueSource is the interface that SetControllerSources requires from the
// job-queue implementation.  Defined here as a narrow interface to avoid an
// import cycle between pkg/observability and cmd/vmafx-controller/queue.
type jobQueueSource interface {
	PendingCount() int
	RunningCount() int
}

// nodeRegistrySource is the interface that SetControllerSources requires from
// the node registry.  Narrow interface — same motivation as jobQueueSource.
type nodeRegistrySource interface {
	Count() int
}

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

// Metrics holds all Prometheus instruments registered by the vmafx-server
// and vmafx-controller.
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
	// JobsSubmitted counts jobs accepted by the controller queue.
	JobsSubmitted prometheus.Counter
	// JobsCompleted counts jobs that finished successfully.
	JobsCompleted prometheus.Counter
	// JobsFailed counts jobs that finished with an error.
	JobsFailed prometheus.Counter
}

// NewMetrics registers and returns the vmafx-server Prometheus metrics.
// reg must be a *prometheus.Registry (use prometheus.NewRegistry() for tests
// or prometheus.DefaultRegisterer for production).
func NewMetrics(reg prometheus.Registerer) *Metrics {
	factory := promauto.With(reg)
	return &Metrics{
		ScoreRequests: factory.NewCounter(prometheus.CounterOpts{
			Namespace: "vmafx",
			Subsystem: "server",
			Name:      "score_requests_total",
			Help:      "Total number of Score requests (HTTP + gRPC).",
		}),
		ScoreErrors: factory.NewCounter(prometheus.CounterOpts{
			Namespace: "vmafx",
			Subsystem: "server",
			Name:      "score_errors_total",
			Help:      "Total number of Score requests that returned an error.",
		}),
		ScoreDuration: factory.NewHistogram(prometheus.HistogramOpts{
			Namespace: "vmafx",
			Subsystem: "server",
			Name:      "score_duration_seconds",
			Help:      "End-to-end duration of a Score request in seconds.",
			Buckets:   prometheus.DefBuckets,
		}),
		HealthRequests: factory.NewCounter(prometheus.CounterOpts{
			Namespace: "vmafx",
			Subsystem: "server",
			Name:      "health_requests_total",
			Help:      "Total number of Health / healthz requests.",
		}),
		ReadyRequests: factory.NewCounter(prometheus.CounterOpts{
			Namespace: "vmafx",
			Subsystem: "server",
			Name:      "ready_requests_total",
			Help:      "Total number of readyz requests.",
		}),
		JobsSubmitted: factory.NewCounter(prometheus.CounterOpts{
			Namespace: "vmafx",
			Subsystem: "controller",
			Name:      "jobs_submitted_total",
			Help:      "Total number of jobs submitted to the controller queue.",
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
			Help:      "Total number of jobs that finished with an error.",
		}),
	}
}

// SetControllerSources registers live-gauge Prometheus metrics backed by the
// job queue and node registry.  It must be called once after NewMetrics and
// before the Prometheus HTTP handler is registered.
//
// The gauges are:
//
//	vmafx_controller_jobs_pending  — current number of pending jobs
//	vmafx_controller_jobs_running  — current number of running jobs
//	vmafx_controller_nodes_live    — current number of registered nodes
//
// SetControllerSources accepts narrow interfaces (jobQueueSource,
// nodeRegistrySource) to avoid an import cycle between pkg/observability and
// the cmd/vmafx-controller sub-packages.
func (m *Metrics) SetControllerSources(q jobQueueSource, r nodeRegistrySource) {
	if q != nil {
		prometheus.MustRegister(prometheus.NewGaugeFunc(prometheus.GaugeOpts{
			Namespace: "vmafx",
			Subsystem: "controller",
			Name:      "jobs_pending",
			Help:      "Current number of PENDING jobs in the queue.",
		}, func() float64 { return float64(q.PendingCount()) }))

		prometheus.MustRegister(prometheus.NewGaugeFunc(prometheus.GaugeOpts{
			Namespace: "vmafx",
			Subsystem: "controller",
			Name:      "jobs_running",
			Help:      "Current number of RUNNING jobs in the queue.",
		}, func() float64 { return float64(q.RunningCount()) }))
	}

	if r != nil {
		prometheus.MustRegister(prometheus.NewGaugeFunc(prometheus.GaugeOpts{
			Namespace: "vmafx",
			Subsystem: "controller",
			Name:      "nodes_live",
			Help:      "Current number of registered (live) vmafx-node instances.",
		}, func() float64 { return float64(r.Count()) }))
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

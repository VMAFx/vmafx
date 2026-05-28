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

// Metrics holds all Prometheus instruments registered by the vmafx-server.
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

// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// pkg/observability/observability.go — structured logging, Prometheus metrics,
// and graceful-shutdown helpers for the vmafx-server.
//
// Logging: Go 1.21 stdlib log/slog, JSON handler, emitted to stdout.
// Metrics: github.com/prometheus/client_golang — per-request counters and
//          latency histogram for both gRPC and HTTP transports, built from
//          the families of pkg/observability/metricdef.
//
// ADR-0703: vmafx-server Go gRPC + HTTP service.
// ADR-1014: Prometheus registry isolation — every family registers on the
//           isolated registry the service serves on /metrics, never on the
//           global DefaultRegisterer.

package observability

import (
	"context"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/collectors"

	"github.com/VMAFx/vmafx/pkg/model"
	"github.com/VMAFx/vmafx/pkg/observability/metricdef"
	"github.com/VMAFx/vmafx/pkg/version"
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

// Metrics holds the Prometheus instruments of the synchronous scoring surface
// (Score and ScoreStream on gRPC, POST /v1/score on HTTP, the probes) that
// vmafx-server and vmafx-controller share, and the quality family. Every
// instrument is built from its family in metricdef; the controller's queue
// families live with the controller (cmd/vmafx-controller/metrics.go).
type Metrics struct {
	// ScoreRequests is the total number of /v1/score / Score RPC calls.
	ScoreRequests prometheus.Counter
	// ScoreErrors is the total number of scoring errors.
	ScoreErrors prometheus.Counter
	// HealthRequests counts /healthz + Health RPC calls.
	HealthRequests prometheus.Counter
	// ReadyRequests counts /readyz calls.
	ReadyRequests prometheus.Counter

	duration Histogram
	quality  Histogram
}

// NewRegistry returns the isolated registry a VMAFx service serves on
// /metrics (ADR-1014), holding the Go runtime and process collectors and
// vmafx_build_info until the service registers its own families.
func NewRegistry() (*prometheus.Registry, error) {
	reg := prometheus.NewRegistry()
	for _, c := range []prometheus.Collector{
		collectors.NewGoCollector(),
		collectors.NewProcessCollector(collectors.ProcessCollectorOpts{}),
	} {
		if err := reg.Register(c); err != nil {
			return nil, fmt.Errorf("observability: register runtime collector: %w", err)
		}
	}
	info, err := NewGauge(reg, metricdef.BuildInfo)
	if err != nil {
		return nil, err
	}
	info.Set(1, version.Version())
	return reg, nil
}

// NewMetrics registers the scoring and quality families on reg, which is the
// service's own registry (ADR-1014: never prometheus.DefaultRegisterer).
func NewMetrics(reg prometheus.Registerer) (*Metrics, error) {
	counters := make([]Counter, 4)
	for i, f := range []metricdef.Family{
		metricdef.ServerScoreRequests, metricdef.ServerScoreErrors,
		metricdef.ServerHealthRequests, metricdef.ServerReadyRequests,
	} {
		c, err := NewCounter(reg, f)
		if err != nil {
			return nil, err
		}
		counters[i] = c
	}
	duration, err := NewHistogram(reg, metricdef.ServerScoreDuration)
	if err != nil {
		return nil, err
	}
	quality, err := NewHistogram(reg, metricdef.QualityScore)
	if err != nil {
		return nil, err
	}
	return &Metrics{
		ScoreRequests:  counters[0].vec.WithLabelValues(),
		ScoreErrors:    counters[1].vec.WithLabelValues(),
		HealthRequests: counters[2].vec.WithLabelValues(),
		ReadyRequests:  counters[3].vec.WithLabelValues(),
		duration:       duration,
		quality:        quality,
	}, nil
}

// ObserveScoreDuration records the duration of one Score request in
// seconds, with the trace of ctx as its exemplar.
func (m *Metrics) ObserveScoreDuration(ctx context.Context, seconds float64) {
	m.duration.ObserveContext(ctx, seconds)
}

// ObserveScore records one pooled score in the quality family. tenant is
// empty on vmafx-server, which has no tenants; modelName is the model the
// request or job named, empty for the default model. The profile label reads
// metricdef.None until scoring requests carry a profile (decision Q-119).
func (m *Metrics) ObserveScore(tenant, modelName string, score float64) {
	if modelName == "" {
		modelName = model.DefaultVersion
	}
	m.quality.Observe(score, tenant, modelName, metricdef.None)
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
	// Use time.NewTimer so the timer is stopped if we return early (e.g. via
	// context cancellation on the outer ctx), preventing a goroutine-timer
	// leak.  ADR-1017.
	t := time.NewTimer(timeout)
	defer t.Stop()
	<-t.C
}

// NewShutdownContext returns a context that is cancelled on SIGTERM / SIGINT.
// The returned stop function MUST be called (typically via `defer`) when the
// context is no longer needed: it both releases the signal handler
// subscription (avoiding a process-lifetime goroutine + signal-handler leak
// when the caller exits without a signal arriving — e.g. early `os.Exit(1)`
// paths in `main`) and cancels the context.
//
// Implementation note: this delegates to `signal.NotifyContext` (Go 1.16+),
// which is the stdlib idiom and unwinds correctly on both paths (signal
// arrival AND `stop()` called first). The previous implementation spawned
// a goroutine blocked on `<-ch` with no `<-ctx.Done()` arm, leaking the
// goroutine + the signal subscription whenever the caller invoked `stop()`
// before any signal fired. ADR-0978.
func NewShutdownContext() (context.Context, context.CancelFunc) {
	return signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT)
}

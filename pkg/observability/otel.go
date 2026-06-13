// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/observability/otel.go — OpenTelemetry trace + meter provider wiring
// for all VMAFX Go services.
//
// Phase 1 (ADR-0927) ships this helper and pilots it in vmafx-controller.
// Subsequent PRs wire vmafx-node, vmafx-server, vmafx-mcp, vmafx-tune one
// at a time.
//
// Usage:
//
//	shutdown := observability.InitOTel(ctx, "vmafx-controller", log)
//	defer shutdown(context.Background())
//
// The returned shutdown function flushes buffered spans + metrics and
// releases the exporter; callers should invoke it via `defer` in main().
//
// Configuration (12-factor §III, OTel-standard env vars):
//
//	OTEL_EXPORTER_OTLP_ENDPOINT  OTLP collector endpoint. When unset, OTel
//	                             initialises with no-op providers and a
//	                             single "tracing disabled" log line is
//	                             emitted. Default for HTTP/protobuf is
//	                             http://localhost:4318; we honour that.
//	OTEL_SERVICE_NAME            Overrides the serviceName argument.
//	OTEL_TRACES_SAMPLER_ARG      Trace sample ratio in [0.0, 1.0]; default 0.01.
//	OTEL_SDK_DISABLED            "true" forces no-op providers regardless
//	                             of endpoint.
//
// Existing slog + Prometheus instrumentation in this package is preserved
// unchanged — OTel is additive.
//
// ADR-0927: OpenTelemetry traces + metrics Phase 1.

package observability

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"strconv"
	"time"

	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/exporters/otlp/otlpmetric/otlpmetrichttp"
	"go.opentelemetry.io/otel/exporters/otlp/otlptrace/otlptracehttp"
	"go.opentelemetry.io/otel/propagation"
	sdkmetric "go.opentelemetry.io/otel/sdk/metric"
	"go.opentelemetry.io/otel/sdk/resource"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	semconv "go.opentelemetry.io/otel/semconv/v1.26.0"
)

// OTelDefaults captures the Phase 1 defaults defined by ADR-0927.
//
//	OTel collector endpoint:   $OTEL_EXPORTER_OTLP_ENDPOINT, no default
//	                           (unset → no-op providers).
//	Trace sample ratio:        $OTEL_TRACES_SAMPLER_ARG, default 0.01 (1 %).
//	Metric export interval:    60 s, matches the Prometheus scrape default.
const (
	// DefaultTraceSampleRatio is the head-based trace sample rate when
	// OTEL_TRACES_SAMPLER_ARG is unset (ADR-0927).
	DefaultTraceSampleRatio = 0.01

	// DefaultMetricExportInterval is the periodic reader interval when
	// the SDK is initialised without explicit overrides (ADR-0927).
	DefaultMetricExportInterval = 60 * time.Second

	// otelEndpointEnv is the standard OTel SDK endpoint variable.
	otelEndpointEnv = "OTEL_EXPORTER_OTLP_ENDPOINT"
	// otelServiceNameEnv overrides the serviceName argument to InitOTel.
	otelServiceNameEnv = "OTEL_SERVICE_NAME"
	// otelTracesSamplerArgEnv overrides DefaultTraceSampleRatio.
	otelTracesSamplerArgEnv = "OTEL_TRACES_SAMPLER_ARG"
	// otelSDKDisabledEnv forces no-op providers when "true".
	otelSDKDisabledEnv = "OTEL_SDK_DISABLED"
)

// OTelShutdown flushes and releases OTel providers. It is safe to call
// with a fresh context; pass a short deadline (e.g. 5 s) to bound
// shutdown work during process exit.
type OTelShutdown func(context.Context) error

// InitOTel sets up the global OTel TracerProvider + MeterProvider for
// serviceName and returns a shutdown function the caller must invoke
// (typically via defer in main()).
//
// When OTEL_EXPORTER_OTLP_ENDPOINT is unset or OTEL_SDK_DISABLED is
// "true", InitOTel installs no-op providers and returns a no-op
// shutdown function — callers do not need to special-case the
// "tracing disabled" path.
//
// serviceName populates the OTel `service.name` resource attribute
// unless OTEL_SERVICE_NAME is set, which wins.
//
// log is used for a single informational line describing the chosen
// configuration; pass nil to suppress (e.g. in tests).
func InitOTel(ctx context.Context, serviceName string, log *slog.Logger) OTelShutdown {
	// Honour OTEL_SDK_DISABLED first — it short-circuits everything.
	if disabled, _ := strconv.ParseBool(os.Getenv(otelSDKDisabledEnv)); disabled {
		logInfo(log, "otel: SDK disabled via OTEL_SDK_DISABLED, providers are no-ops",
			"service", serviceName)
		return noopShutdown
	}

	endpoint := os.Getenv(otelEndpointEnv)
	if endpoint == "" {
		logInfo(log, "otel: no endpoint configured, tracing disabled",
			"hint", "set "+otelEndpointEnv+" (e.g. http://localhost:4318) to enable")
		return noopShutdown
	}

	// Resolve service name — env wins over caller argument (OTel convention).
	resolvedName := serviceName
	if envName := os.Getenv(otelServiceNameEnv); envName != "" {
		resolvedName = envName
	}

	// Resolve sample ratio — env wins, else default.
	sampleRatio := DefaultTraceSampleRatio
	if raw := os.Getenv(otelTracesSamplerArgEnv); raw != "" {
		if parsed, err := strconv.ParseFloat(raw, 64); err == nil && parsed >= 0 && parsed <= 1 {
			sampleRatio = parsed
		}
	}

	res, err := buildResource(ctx, resolvedName)
	if err != nil {
		logInfo(log, "otel: failed to build resource, falling back to no-op",
			"error", err.Error())
		return noopShutdown
	}

	tp, traceShutdown, err := buildTracerProvider(ctx, res, sampleRatio)
	if err != nil {
		logInfo(log, "otel: tracer provider init failed, traces disabled",
			"error", err.Error())
		traceShutdown = noopShutdown
	} else {
		otel.SetTracerProvider(tp)
		otel.SetTextMapPropagator(propagation.NewCompositeTextMapPropagator(
			propagation.TraceContext{},
			propagation.Baggage{},
		))
	}

	mp, metricShutdown, err := buildMeterProvider(ctx, res)
	if err != nil {
		logInfo(log, "otel: meter provider init failed, OTel metrics disabled",
			"error", err.Error())
		metricShutdown = noopShutdown
	} else {
		otel.SetMeterProvider(mp)
	}

	logInfo(log, "otel: initialised",
		"service", resolvedName,
		"endpoint", endpoint,
		"trace_sample_ratio", sampleRatio,
		"metric_export_interval", DefaultMetricExportInterval.String(),
	)

	// Compose shutdown — invoke both, return the joined error.
	return func(shutdownCtx context.Context) error {
		return errors.Join(
			traceShutdown(shutdownCtx),
			metricShutdown(shutdownCtx),
		)
	}
}

// buildResource constructs the OTel resource describing the running
// service (service.name + telemetry.sdk.* attributes via Default).
func buildResource(ctx context.Context, serviceName string) (*resource.Resource, error) {
	return resource.New(ctx,
		resource.WithAttributes(semconv.ServiceName(serviceName)),
		resource.WithFromEnv(),
		resource.WithTelemetrySDK(),
		resource.WithProcess(),
	)
}

// buildTracerProvider returns a configured SDK TracerProvider, an
// OTelShutdown that flushes and shuts it down, and any setup error.
func buildTracerProvider(
	ctx context.Context,
	res *resource.Resource,
	sampleRatio float64,
) (*sdktrace.TracerProvider, OTelShutdown, error) {
	exporter, err := otlptracehttp.New(ctx)
	if err != nil {
		return nil, nil, fmt.Errorf("otlp trace exporter: %w", err)
	}
	tp := sdktrace.NewTracerProvider(
		sdktrace.WithBatcher(exporter),
		sdktrace.WithResource(res),
		sdktrace.WithSampler(sdktrace.ParentBased(
			sdktrace.TraceIDRatioBased(sampleRatio),
		)),
	)
	return tp, tp.Shutdown, nil
}

// buildMeterProvider returns a configured SDK MeterProvider, an
// OTelShutdown that flushes and shuts it down, and any setup error.
func buildMeterProvider(
	ctx context.Context,
	res *resource.Resource,
) (*sdkmetric.MeterProvider, OTelShutdown, error) {
	exporter, err := otlpmetrichttp.New(ctx)
	if err != nil {
		return nil, nil, fmt.Errorf("otlp metric exporter: %w", err)
	}
	mp := sdkmetric.NewMeterProvider(
		sdkmetric.WithReader(sdkmetric.NewPeriodicReader(exporter,
			sdkmetric.WithInterval(DefaultMetricExportInterval),
		)),
		sdkmetric.WithResource(res),
	)
	return mp, mp.Shutdown, nil
}

// noopShutdown is the OTelShutdown returned when OTel is disabled or
// fails to initialise. It always returns nil.
func noopShutdown(_ context.Context) error { return nil }

// logInfo emits a single info-level slog record if log is non-nil.
// Tests pass nil to keep the package quiet under `go test`.
func logInfo(log *slog.Logger, msg string, args ...any) {
	if log == nil {
		return
	}
	log.Info(msg, args...)
}

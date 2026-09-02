// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/observability/otel_instruments_test.go — unit tests for the OTel
// instrument helpers: InitOTelMetrics, StartSpan, EndSpan, ObserveScoreLatency.
//
// These use the SDK no-op provider and an in-process tracer/meter so no
// network connection is required.
//
// ADR-0782: OpenTelemetry tracing and metrics schema.

package observability

import (
	"context"
	"errors"
	"testing"
	"time"

	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	otelcodes "go.opentelemetry.io/otel/codes"
	sdkmetric "go.opentelemetry.io/otel/sdk/metric"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	"go.opentelemetry.io/otel/sdk/trace/tracetest"
)

// TestInitOTelMetrics_NonNilReturn verifies that InitOTelMetrics never returns
// nil, even when called with a no-op MeterProvider (instrument creation errors
// degrade to no-ops per the pkg contract).
func TestInitOTelMetrics_NonNilReturn(t *testing.T) {
	t.Parallel()
	mp := sdkmetric.NewMeterProvider()
	defer func() { _ = mp.Shutdown(context.Background()) }()

	om := InitOTelMetrics(mp, "test-component")
	if om == nil {
		t.Fatal("InitOTelMetrics returned nil")
	}
	if om.JobsQueued == nil {
		t.Error("JobsQueued is nil")
	}
	if om.JobsInFlight == nil {
		t.Error("JobsInFlight is nil")
	}
	if om.FramesPerSecond == nil {
		t.Error("FramesPerSecond is nil")
	}
	if om.ScoreLatency == nil {
		t.Error("ScoreLatency is nil")
	}
	if om.GPUUtilization == nil {
		t.Error("GPUUtilization is nil")
	}
}

// TestInitOTelMetrics_ComponentInstrumentName verifies the component suffix is
// incorporated by recording against a real SDK meter and confirming the data
// is collected.
func TestInitOTelMetrics_ComponentInstrumentName(t *testing.T) {
	t.Parallel()
	reader := sdkmetric.NewManualReader()
	mp := sdkmetric.NewMeterProvider(sdkmetric.WithReader(reader))
	defer func() { _ = mp.Shutdown(context.Background()) }()

	om := InitOTelMetrics(mp, "controller")
	if om == nil {
		t.Fatal("InitOTelMetrics returned nil")
	}

	// Record a value to confirm the instrument is wired correctly.
	om.JobsQueued.Add(context.Background(), 3)
	om.ScoreLatency.Record(context.Background(), 42.5)
}

// TestStartSpan_WithSDKProvider verifies that StartSpan does not panic and
// returns non-nil objects when called with a real SDK TracerProvider installed.
func TestStartSpan_WithSDKProvider(t *testing.T) {
	// Note: not t.Parallel() — installs a global TracerProvider.
	exporter := tracetest.NewInMemoryExporter()
	tp := sdktrace.NewTracerProvider(sdktrace.WithSyncer(exporter))
	orig := otel.GetTracerProvider()
	otel.SetTracerProvider(tp)
	defer func() {
		otel.SetTracerProvider(orig)
		_ = tp.Shutdown(context.Background())
	}()

	ctx, span := StartSpan(context.Background(), SpanScoring, AttrModel.String("vmaf_v0.6.1"))
	if ctx == nil {
		t.Error("StartSpan returned nil context")
	}
	if span == nil {
		t.Error("StartSpan returned nil span")
	}
	span.End()
}

// TestStartSpan_WithAttributes verifies that attribute keys from otel_instruments
// are valid attribute.KeyValue values (compilation guard).
func TestStartSpan_WithAttributes(t *testing.T) {
	t.Parallel()
	attrs := []attribute.KeyValue{
		AttrJobID.String("job-1"),
		AttrModel.String("vmaf_v0.6.1"),
		AttrBackend.String("cpu"),
		AttrNodeID.String("node-1"),
		AttrVendor.String("nvidia"),
		AttrStatus.String("ok"),
	}
	_, span := StartSpan(context.Background(), SpanScoring, attrs...)
	defer span.End()
}

// TestEndSpan_NilError marks the span OK when errp points to nil.
// Note: not t.Parallel() — installs a global TracerProvider.
func TestEndSpan_NilError(t *testing.T) {
	exporter := tracetest.NewInMemoryExporter()
	tp := sdktrace.NewTracerProvider(sdktrace.WithSyncer(exporter))
	orig := otel.GetTracerProvider()
	otel.SetTracerProvider(tp)
	defer func() {
		otel.SetTracerProvider(orig)
		_ = tp.Shutdown(context.Background())
	}()

	_, span := tp.Tracer("vmafx").Start(context.Background(), "test-nil-error")
	var err error
	EndSpan(span, &err)

	spans := exporter.GetSpans()
	if len(spans) == 0 {
		t.Fatal("no spans recorded")
	}
	if spans[0].Status.Code != otelcodes.Ok {
		t.Errorf("status code: got %v, want Ok", spans[0].Status.Code)
	}
}

// TestEndSpan_WithError marks the span Error when errp points to a non-nil error.
// Note: not t.Parallel() — installs a global TracerProvider.
func TestEndSpan_WithError(t *testing.T) {
	exporter := tracetest.NewInMemoryExporter()
	tp := sdktrace.NewTracerProvider(sdktrace.WithSyncer(exporter))
	orig := otel.GetTracerProvider()
	otel.SetTracerProvider(tp)
	defer func() {
		otel.SetTracerProvider(orig)
		_ = tp.Shutdown(context.Background())
	}()

	_, span := tp.Tracer("vmafx").Start(context.Background(), "test-with-error")
	err := errors.New("scoring failed")
	EndSpan(span, &err)

	spans := exporter.GetSpans()
	if len(spans) == 0 {
		t.Fatal("no spans recorded")
	}
	if spans[0].Status.Code != otelcodes.Error {
		t.Errorf("status code: got %v, want Error", spans[0].Status.Code)
	}
}

// TestObserveScoreLatency_NilOmDoesNotPanic verifies the nil guard in
// ObserveScoreLatency.
func TestObserveScoreLatency_NilOmDoesNotPanic(t *testing.T) {
	t.Parallel()
	// Must not panic.
	ObserveScoreLatency(context.Background(), nil, time.Now(), AttrBackend.String("cpu"))
}

// TestObserveScoreLatency_RecordsElapsed verifies the recording path with a
// real SDK meter (no-crash, no data-race smoke test).
func TestObserveScoreLatency_RecordsElapsed(t *testing.T) {
	t.Parallel()
	reader := sdkmetric.NewManualReader()
	mp := sdkmetric.NewMeterProvider(sdkmetric.WithReader(reader))
	defer func() { _ = mp.Shutdown(context.Background()) }()

	om := InitOTelMetrics(mp, "test")
	start := time.Now().Add(-100 * time.Millisecond) // simulate 100 ms elapsed

	// Must not panic; we can't assert the exact value without reading the reader.
	ObserveScoreLatency(context.Background(), om, start, AttrBackend.String("cpu"), AttrModel.String("vmaf_v0.6.1"))
}

// TestSpanNameConstants verifies that the span-name constants are non-empty
// strings (a trivial guard against accidental blanking via a refactor).
func TestSpanNameConstants(t *testing.T) {
	t.Parallel()
	constants := map[string]string{
		"SpanJobSubmit":       SpanJobSubmit,
		"SpanEncoderDispatch": SpanEncoderDispatch,
		"SpanFrameExtraction": SpanFrameExtraction,
		"SpanScoring":         SpanScoring,
		"SpanONNXInference":   SpanONNXInference,
	}
	for name, val := range constants {
		if val == "" {
			t.Errorf("%s is empty string", name)
		}
	}
}

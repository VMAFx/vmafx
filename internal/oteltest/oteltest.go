// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package oteltest is the test-only seam for asserting that a vmafx request
// path emits the OpenTelemetry span ADR-0782 promises. It installs an SDK
// TracerProvider backed by an in-memory span recorder as the global provider
// for the duration of one test, which is exactly the slot golusoris's
// otel.Module leaves alone when no OTLP endpoint is configured (ADR-1119) —
// so a production composition root started with the endpoint unset records
// its spans here instead of dropping them, and the test can read them back
// without a collector.
//
// It is shared by every cmd/ binary's test package and by pkg/ai so the
// recorder discipline (install, restore, no parallelism) is written once.
package oteltest

import (
	"testing"

	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/propagation"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	"go.opentelemetry.io/otel/sdk/trace/tracetest"
	"go.opentelemetry.io/otel/trace/noop"
)

// Recorder installs an always-sampling SDK TracerProvider that feeds every
// ended span into the returned recorder, plus the W3C TraceContext + Baggage
// propagator golusoris installs on its active path, and parks no-op globals
// when the test ends. The globals are process-wide state, so callers must
// not use t.Parallel().
//
// Both globals are installed before the caller builds its fx graph:
// golusoris's otel.Module only replaces them on the active path (endpoint
// configured), so with OTEL_EXPORTER_OTLP_ENDPOINT unset the recorder stays
// in place and otelgrpc / otelhttp / observability.StartSpan all report to
// it. The propagator matters for the cross-process assertions: without it the
// otelgrpc client handler has nothing to inject and a server span would show
// up as an unparented root, which is exactly the ADR-1095 defect the
// propagation tests exist to catch.
func Recorder(t *testing.T) *tracetest.SpanRecorder {
	t.Helper()
	NoopEnv(t)

	sr := tracetest.NewSpanRecorder()
	tp := sdktrace.NewTracerProvider(
		sdktrace.WithSampler(sdktrace.AlwaysSample()),
		sdktrace.WithSpanProcessor(sr),
	)
	otel.SetTracerProvider(tp)
	otel.SetTextMapPropagator(propagation.NewCompositeTextMapPropagator(
		propagation.TraceContext{},
		propagation.Baggage{},
	))
	t.Cleanup(func() {
		// The original globals cannot be reinstalled once swapped (the SDK
		// refuses to re-register its delegates), so park no-op ones.
		otel.SetTracerProvider(noop.NewTracerProvider())
		otel.SetTextMapPropagator(propagation.NewCompositeTextMapPropagator())
	})
	return sr
}

// NoopEnv pins golusoris's otel.Module onto its silent no-op path for the
// duration of the test — no OTLP endpoint via any of the standard
// OTEL_EXPORTER_OTLP_*_ENDPOINT variables or the vmafx config key, and the
// OTEL_SDK_DISABLED kill switch cleared — so an operator's shell environment
// cannot flip a wiring test onto a real exporter, and so the module leaves the
// global TracerProvider (the Recorder's slot) untouched.
func NoopEnv(t *testing.T) {
	t.Helper()
	for _, k := range []string{
		"OTEL_EXPORTER_OTLP_ENDPOINT",
		"OTEL_EXPORTER_OTLP_TRACES_ENDPOINT",
		"OTEL_EXPORTER_OTLP_METRICS_ENDPOINT",
		"OTEL_EXPORTER_OTLP_LOGS_ENDPOINT",
		"OTEL_SDK_DISABLED",
		"VMAFX_OTEL_ENDPOINT",
	} {
		t.Setenv(k, "")
	}
}

// Ended returns the recorder's ended spans that carry name, in end order.
func Ended(sr *tracetest.SpanRecorder, name string) []sdktrace.ReadOnlySpan {
	var out []sdktrace.ReadOnlySpan
	for _, s := range sr.Ended() {
		if s.Name() == name {
			out = append(out, s)
		}
	}
	return out
}

// Names lists the names of every ended span, for failure messages.
func Names(sr *tracetest.SpanRecorder) []string {
	spans := sr.Ended()
	out := make([]string, 0, len(spans))
	for _, s := range spans {
		out = append(out, s.Name())
	}
	return out
}

// HasAttr reports whether span carries the string attribute key=value.
func HasAttr(span sdktrace.ReadOnlySpan, key attribute.Key, value string) bool {
	for _, kv := range span.Attributes() {
		if kv.Key == key && kv.Value.AsString() == value {
			return true
		}
	}
	return false
}

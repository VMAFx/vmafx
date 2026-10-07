// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package bootstrap

import (
	"bytes"
	"context"
	"log/slog"
	"strings"
	"sync"
	"testing"

	corelog "github.com/golusoris/golusoris/core/log"
	"github.com/golusoris/golusoris/otel"
	sdklog "go.opentelemetry.io/otel/sdk/log"
	"go.opentelemetry.io/otel/trace"
	"go.uber.org/fx"
	"go.uber.org/fx/fxtest"
)

// memExporter keeps the log records the provider exports.
type memExporter struct {
	mu      sync.Mutex
	records []sdklog.Record
}

func (e *memExporter) Export(_ context.Context, recs []sdklog.Record) error {
	e.mu.Lock()
	defer e.mu.Unlock()
	for _, r := range recs {
		e.records = append(e.records, r.Clone())
	}
	return nil
}

func (e *memExporter) Shutdown(context.Context) error   { return nil }
func (e *memExporter) ForceFlush(context.Context) error { return nil }

// spanContext is a context carrying a fixed, sampled span.
func spanContext(t *testing.T) (context.Context, trace.SpanContext) {
	t.Helper()
	tid, err := trace.TraceIDFromHex("4bf92f3577b34da6a3ce929d0e0e4736")
	if err != nil {
		t.Fatal(err)
	}
	sid, err := trace.SpanIDFromHex("00f067aa0ba902b7")
	if err != nil {
		t.Fatal(err)
	}
	sc := trace.NewSpanContext(trace.SpanContextConfig{TraceID: tid, SpanID: sid, TraceFlags: trace.FlagsSampled})
	return trace.ContextWithSpanContext(context.Background(), sc), sc
}

// TestInjectedLoggerIsBridgedToOTel: a record written through the
// *slog.Logger a service gets from the graph reaches the OTel logger provider
// with the trace and span of its context (Q-110; golusoris#617: the bridge
// must decorate the injected logger, not only slog.Default). Not parallel:
// the bridge sets slog.Default.
func TestInjectedLoggerIsBridgedToOTel(t *testing.T) {
	prev := slog.Default()
	t.Cleanup(func() { slog.SetDefault(prev) })
	exp := &memExporter{}
	provider := sdklog.NewLoggerProvider(sdklog.WithProcessor(sdklog.NewSimpleProcessor(exp)))
	ctx, sc := spanContext(t)
	var out bytes.Buffer
	app := fxtest.New(t,
		fx.Supply(&otel.Providers{Logger: provider}),
		fx.Supply(otel.Options{Enabled: true, Service: otel.ServiceOptions{Name: "vmafx-test"}}),
		fx.Provide(func() *slog.Logger { return slog.New(slog.NewJSONHandler(&out, nil)) }),
		otel.ModuleWithSlogBridge,
		fx.Invoke(func(l *slog.Logger) {
			l.InfoContext(ctx, "from the injected logger")
			slog.Default().InfoContext(ctx, "from slog.Default")
		}),
	)
	app.RequireStart()
	app.RequireStop()

	// The control: the bridge itself works for slog.Default.
	if !exported(exp, "from slog.Default", sc) {
		t.Fatalf("the bridge exported nothing for slog.Default: the harness is broken")
	}
	if !exported(exp, "from the injected logger", sc) {
		t.Errorf("the injected logger's record did not reach the OTel provider with its trace (golusoris#617)")
	}
}

// exported reports whether exp holds a record with body and the span sc.
func exported(exp *memExporter, body string, sc trace.SpanContext) bool {
	exp.mu.Lock()
	defer exp.mu.Unlock()
	for _, r := range exp.records {
		if r.Body().AsString() == body && r.TraceID() == sc.TraceID() && r.SpanID() == sc.SpanID() {
			return true
		}
	}
	return false
}

// TestLogLinesCarryTraceAndSpan: a JSON log line written inside a span names
// the trace and the span, so a log backend joins it with its trace (Q-110;
// golusoris#618).
func TestLogLinesCarryTraceAndSpan(t *testing.T) {
	t.Parallel()
	var out bytes.Buffer
	logger := corelog.New(corelog.Options{Format: corelog.FormatJSON, Output: &out})
	ctx, sc := spanContext(t)
	logger.InfoContext(ctx, "inside a span")
	line := out.String()
	for _, want := range []string{`"trace_id":"` + sc.TraceID().String() + `"`, `"span_id":"` + sc.SpanID().String() + `"`} {
		if !strings.Contains(line, want) {
			t.Errorf("log line %q lacks %s (golusoris#618)", strings.TrimSpace(line), want)
		}
	}
}

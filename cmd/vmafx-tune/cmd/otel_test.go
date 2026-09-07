// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// otel_test.go — OpenTelemetry wiring of the vmafx-tune CLI (ADR-0782,
// ADR-1119). Every subcommand runs through withGolusoris, which builds the
// shared bootstrap.Base graph (golusoris otel.Module) per invocation and wraps
// the domain function in one SpanTuneCommand span. These tests prove both
// halves without a collector: the no-op path when no OTLP endpoint is set, and
// the span itself via an in-memory recorder.
//
// Not t.Parallel(): both tests touch process-global state (env, tracer).

package cmd

import (
	"context"
	"errors"
	"testing"

	"github.com/spf13/cobra"
	"go.opentelemetry.io/otel/codes"
	"go.opentelemetry.io/otel/trace"

	"github.com/VMAFx/vmafx/internal/oteltest"
	"github.com/VMAFx/vmafx/pkg/observability"
)

func TestWithGolusoris_OTelWiredNoopWithoutEndpoint(t *testing.T) {
	oteltest.NoopEnv(t)
	t.Setenv("VMAFX_LOG_LEVEL", "error")

	var got deps
	run := withGolusoris(func(_ context.Context, d deps, _ []string) error {
		got = d
		return nil
	})
	c := &cobra.Command{Use: "probe"}
	c.SetContext(context.Background())
	if err := run(c, nil); err != nil {
		t.Fatalf("withGolusoris run: %v", err)
	}
	if got.OTel == nil {
		t.Fatal("deps.OTel is nil: bootstrap.Base's otel.Module is not in the vmafx-tune graph")
	}
	if got.OTel.Tracer != nil || got.OTel.Meter != nil || got.OTel.Logger != nil {
		t.Errorf("expected no-op providers without an OTLP endpoint, got %+v", got.OTel)
	}
	if got.Log == nil || got.Cfg == nil {
		t.Errorf("framework deps missing alongside OTel: log=%v cfg=%v", got.Log != nil, got.Cfg != nil)
	}
}

func TestWithGolusoris_CommandSpanWrapsRun(t *testing.T) {
	t.Setenv("VMAFX_LOG_LEVEL", "error")
	sr := oteltest.Recorder(t)

	errBoom := errors.New("boom")
	sub := &cobra.Command{
		Use: "probe",
		RunE: withGolusoris(func(ctx context.Context, _ deps, _ []string) error {
			if !trace.SpanContextFromContext(ctx).IsValid() {
				return errors.New("domain function ran outside the command span")
			}
			return errBoom
		}),
	}
	root := &cobra.Command{Use: "vmafx-tune-go", SilenceUsage: true, SilenceErrors: true}
	root.AddCommand(sub)
	root.SetArgs([]string{"probe"})

	err := root.ExecuteContext(context.Background())
	if !errors.Is(err, errBoom) {
		t.Fatalf("domain error not propagated through the span wrapper: %v", err)
	}

	spans := oteltest.Ended(sr, observability.SpanTuneCommand)
	if len(spans) != 1 {
		t.Fatalf("want one %s span, got %v", observability.SpanTuneCommand, oteltest.Names(sr))
	}
	span := spans[0]
	if !oteltest.HasAttr(span, observability.AttrTuneCommand, "vmafx-tune-go probe") {
		t.Errorf("span attributes %v lack %s=%q", span.Attributes(), observability.AttrTuneCommand, "vmafx-tune-go probe")
	}
	if span.Status().Code != codes.Error {
		t.Errorf("span status %v, want Error for a failed command", span.Status())
	}
}

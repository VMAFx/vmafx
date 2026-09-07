// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-mcp/otel_test.go — OpenTelemetry wiring of the MCP binary
// (ADR-0782, ADR-1119): the composition root inherits golusoris's otel.Module
// from bootstrap.Base (no-op without an OTLP endpoint, service identity from
// the binary + pkg/version), and every tool call runs inside one SpanMCPTool
// span (addRawTool). The span test drives a real tool over the SDK's in-memory
// transports and reads the span back from an in-memory recorder, so no
// collector is involved.
//
// Not t.Parallel(): both tests touch process-global state (env, tracer).

package main

import (
	"context"
	"encoding/json"
	"testing"

	"github.com/golusoris/golusoris/config"
	"github.com/golusoris/golusoris/otel"
	"github.com/modelcontextprotocol/go-sdk/mcp"
	"go.uber.org/fx"

	"github.com/VMAFx/vmafx/internal/app/bootstrap"
	"github.com/VMAFx/vmafx/internal/oteltest"
	"github.com/VMAFx/vmafx/pkg/observability"
	buildversion "github.com/VMAFx/vmafx/pkg/version"
)

// TestOTelWiredThroughBootstrap builds the production graph minus the
// transport invoke (which would start serving stdio) and asserts the OTel
// contract bootstrap.Base delivers to vmafx-mcp.
func TestOTelWiredThroughBootstrap(t *testing.T) {
	oteltest.NoopEnv(t)
	t.Setenv("VMAFX_LOG_LEVEL", "error")
	t.Setenv("OTEL_SERVICE_NAME", "")
	t.Setenv("VMAFX_OTEL_SERVICE_NAME", "")
	t.Setenv("VMAFX_OTEL_SERVICE_VERSION", "")

	var (
		providers *otel.Providers
		opts      otel.Options
		srv       *mcp.Server
	)
	app := fx.New(
		bootstrap.Base,
		fx.Replace(config.Options{EnvPrefix: "VMAFX_", Delimiter: ".", Watch: false}),
		fx.NopLogger,
		fx.Provide(buildMCPServer),
		fx.Populate(&providers, &opts, &srv),
	)
	if err := app.Err(); err != nil {
		t.Fatalf("vmafx-mcp bootstrap graph: %v", err)
	}
	if srv == nil {
		t.Fatal("buildMCPServer did not provide *mcp.Server")
	}
	if providers == nil || providers.Tracer != nil || providers.Meter != nil || providers.Logger != nil {
		t.Fatalf("expected no-op OTel providers without an endpoint, got %+v", providers)
	}
	if opts.Service.Name != "vmafx-mcp" {
		t.Errorf("service.name = %q, want vmafx-mcp", opts.Service.Name)
	}
	if opts.Service.Version != buildversion.Version() {
		t.Errorf("service.version = %q, want %q", opts.Service.Version, buildversion.Version())
	}
}

// TestToolCallEmitsSpan calls a tool end to end (client → in-memory transport
// → server → addRawTool → handler) and asserts the SpanMCPTool span with the
// tool name attribute was recorded.
func TestToolCallEmitsSpan(t *testing.T) {
	sr := oteltest.Recorder(t)

	srv := buildServer(nil)
	client := mcp.NewClient(&mcp.Implementation{Name: "otel-test-client", Version: "0.0.1"}, nil)
	t1, t2 := mcp.NewInMemoryTransports()
	ctx := context.Background()
	if _, err := srv.Connect(ctx, t1, nil); err != nil {
		t.Fatalf("server.Connect: %v", err)
	}
	session, err := client.Connect(ctx, t2, nil)
	if err != nil {
		t.Fatalf("client.Connect: %v", err)
	}
	defer session.Close()

	result, err := session.CallTool(ctx, &mcp.CallToolParams{
		Name:      "list_backends",
		Arguments: json.RawMessage(`{}`),
	})
	if err != nil {
		t.Fatalf("CallTool list_backends: %v", err)
	}
	if result == nil {
		t.Fatal("CallTool returned a nil result")
	}

	spans := oteltest.Ended(sr, observability.SpanMCPTool)
	if len(spans) != 1 {
		t.Fatalf("want one %s span, got %v", observability.SpanMCPTool, oteltest.Names(sr))
	}
	if !oteltest.HasAttr(spans[0], observability.AttrMCPTool, "list_backends") {
		t.Errorf("span attributes %v lack %s=list_backends", spans[0].Attributes(), observability.AttrMCPTool)
	}
}

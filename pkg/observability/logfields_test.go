// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package observability

import (
	"bytes"
	"context"
	"encoding/json"
	"log/slog"
	"testing"
	"time"

	"go.opentelemetry.io/otel/trace"
)

func TestLogFieldNamesAreFrozen(t *testing.T) {
	t.Parallel()
	want := map[string]string{
		FieldRequestID: "request_id", FieldRPC: "rpc", FieldRoute: "route",
		FieldDurationS: "duration_s", FieldModel: "model", FieldBackend: "backend",
		FieldError: "error",
	}
	for got, name := range want {
		if got != name {
			t.Errorf("field name %q, want %q", got, name)
		}
	}
}

func TestRequestIDUsesTraceIDWhenSpanPresent(t *testing.T) {
	t.Parallel()
	tid, _ := trace.TraceIDFromHex("0102030405060708090a0b0c0d0e0f10")
	sid, _ := trace.SpanIDFromHex("0102030405060708")
	ctx := trace.ContextWithSpanContext(context.Background(),
		trace.NewSpanContext(trace.SpanContextConfig{TraceID: tid, SpanID: sid}))
	if got := RequestID(ctx); got != tid.String() {
		t.Errorf("RequestID = %q, want trace id %q", got, tid)
	}
}

func TestRequestIDRandomWithoutSpan(t *testing.T) {
	t.Parallel()
	a, b := RequestID(context.Background()), RequestID(context.Background())
	if len(a) != 2*requestIDBytes || a == b {
		t.Errorf("ids %q / %q: want two distinct %d-char values", a, b, 2*requestIDBytes)
	}
}

func decodeLine(t *testing.T, buf *bytes.Buffer) map[string]any {
	t.Helper()
	var m map[string]any
	if err := json.Unmarshal(buf.Bytes(), &m); err != nil {
		t.Fatalf("log line is not JSON: %v: %s", err, buf.String())
	}
	return m
}

func TestRPCAndRouteLoggersCarryTheFieldSet(t *testing.T) {
	t.Parallel()
	var buf bytes.Buffer
	base := slog.New(slog.NewJSONHandler(&buf, nil))

	RPCLogger(context.Background(), base, "Score").
		Info("done", FieldModel, "vmaf_v0.6.1", Seconds(1500*time.Millisecond))
	m := decodeLine(t, &buf)
	for _, k := range []string{FieldRequestID, FieldRPC, FieldModel, FieldDurationS} {
		if _, ok := m[k]; !ok {
			t.Errorf("rpc line lacks %q: %v", k, m)
		}
	}
	if m[FieldRPC] != "Score" || m[FieldDurationS] != 1.5 {
		t.Errorf("rpc line values wrong: %v", m)
	}

	buf.Reset()
	RouteLogger(context.Background(), base, "POST", "/v1/score").Warn("rejected")
	m = decodeLine(t, &buf)
	if m[FieldRoute] != "POST /v1/score" || m[FieldRequestID] == "" {
		t.Errorf("route line wrong: %v", m)
	}
}

func TestLoggersTolerateNilLogger(t *testing.T) {
	t.Parallel()
	if RPCLogger(context.Background(), nil, "Health") == nil || RouteLogger(context.Background(), nil, "GET", "/x") == nil {
		t.Error("nil logger must fall back to slog.Default()")
	}
}

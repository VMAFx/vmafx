// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package observability

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"log/slog"
	"time"

	"go.opentelemetry.io/otel/trace"
)

// Structured-log field names shared by every vmafx server log line (issue
// #1251, "structured logging with consistent fields"). A field that appears on
// a log line uses exactly one of these keys, so one query finds the same thing
// on the gRPC, REST and legacy HTTP paths.
const (
	// FieldRequestID identifies one request across every line it logs. It is
	// the OpenTelemetry trace id when the request carries a span, otherwise a
	// random 64-bit hex value.
	FieldRequestID = "request_id"
	// FieldRPC is the gRPC method name without the package: "Score",
	// "ScoreStream", "Health".
	FieldRPC = "rpc"
	// FieldRoute is the HTTP route as "<METHOD> <path>", for example
	// "POST /v1/score". It is the route pattern, never a raw URL with a query.
	FieldRoute = "route"
	// FieldDurationS is an elapsed time in seconds as a float.
	FieldDurationS = "duration_s"
	// FieldModel is the VMAF model name the request asked for.
	FieldModel = "model"
	// FieldBackend is the compute backend: cpu, cuda, sycl, hip, metal.
	FieldBackend = "backend"
	// FieldError carries an error value.
	FieldError = "error"
)

// requestIDBytes is the size of a generated request id before hex encoding.
const requestIDBytes = 8

// RequestID returns the id that ties a request's log lines together: the trace
// id of the span in ctx when there is one, otherwise a fresh random id. An
// empty string is returned only when the system random source fails.
func RequestID(ctx context.Context) string {
	if sc := trace.SpanContextFromContext(ctx); sc.HasTraceID() {
		return sc.TraceID().String()
	}
	var b [requestIDBytes]byte
	if _, err := rand.Read(b[:]); err != nil {
		return ""
	}
	return hex.EncodeToString(b[:])
}

// RPCLogger returns log with the request id and gRPC method attached. A nil
// log selects slog.Default().
func RPCLogger(ctx context.Context, log *slog.Logger, rpc string) *slog.Logger {
	return orDefault(log).With(FieldRequestID, RequestID(ctx), FieldRPC, rpc)
}

// RouteLogger returns log with the request id and HTTP route attached. A nil
// log selects slog.Default().
func RouteLogger(ctx context.Context, log *slog.Logger, method, route string) *slog.Logger {
	return orDefault(log).With(FieldRequestID, RequestID(ctx), FieldRoute, method+" "+route)
}

// Seconds is the duration_s value for d.
func Seconds(d time.Duration) slog.Attr {
	return slog.Float64(FieldDurationS, d.Seconds())
}

func orDefault(log *slog.Logger) *slog.Logger {
	if log == nil {
		return slog.Default()
	}
	return log
}

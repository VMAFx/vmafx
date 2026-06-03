// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/observability/otel_extra_test.go — additional branch coverage for
// otel.go paths not reached by otel_test.go:
//
//   - logInfo with a non-nil logger emits a record (vs. nil-logger no-op).
//   - InitOTel with a bad OTEL_TRACES_SAMPLER_ARG string (non-numeric) falls
//     back to DefaultTraceSampleRatio rather than crashing.
//   - noopShutdown always returns nil.
//
// ADR-0927: OpenTelemetry traces + metrics Phase 1.

package observability

import (
	"context"
	"bytes"
	"log/slog"
	"testing"
)

// TestLogInfo_WithLogger verifies logInfo emits a record when log is non-nil.
// We construct a buffer-backed handler to intercept the output.
func TestLogInfo_WithLogger(t *testing.T) {
	t.Parallel()
	var buf bytes.Buffer
	handler := slog.NewJSONHandler(&buf, &slog.HandlerOptions{Level: slog.LevelDebug})
	lg := slog.New(handler)

	logInfo(lg, "test message", "key", "value")

	if buf.Len() == 0 {
		t.Error("expected log output, but buffer is empty")
	}
	out := buf.String()
	if !bytes.Contains([]byte(out), []byte("test message")) {
		t.Errorf("expected 'test message' in output, got: %s", out)
	}
}

// TestLogInfo_NilLogger verifies logInfo is a no-op (no panic) when log is nil.
func TestLogInfo_NilLogger(t *testing.T) {
	t.Parallel()
	// Must not panic.
	logInfo(nil, "should be silent", "k", "v")
}

// TestNoopShutdown_ReturnsNil verifies the no-op shutdown function always
// returns nil regardless of the context state.
func TestNoopShutdown_ReturnsNil(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	if err := noopShutdown(ctx); err != nil {
		t.Errorf("noopShutdown returned non-nil error: %v", err)
	}
}

// TestInitOTel_InvalidSamplerArgString verifies that a non-numeric
// OTEL_TRACES_SAMPLER_ARG (e.g. "bad") is silently ignored and the SDK still
// initialises using DefaultTraceSampleRatio.  The returned shutdown must be
// non-nil and callable without panic.
func TestInitOTel_InvalidSamplerArgString(t *testing.T) {
	// Not parallel — installs global OTel providers.
	resetGlobals()

	withEnv(t,
		"OTEL_EXPORTER_OTLP_ENDPOINT", "http://127.0.0.1:14318",
		"OTEL_TRACES_SAMPLER_ARG", "not-a-number",
	)

	sd := InitOTel(context.Background(), "test-service", nil)
	if sd == nil {
		t.Fatal("InitOTel returned nil shutdown")
	}
	defer shutdownBounded(t, sd)
}

// TestInitOTel_CompositeShutdown verifies that the composite shutdown function
// returned by InitOTel (when both providers are built) returns without panicking
// when called with a cancelled context.
func TestInitOTel_CompositeShutdown_CancelledCtx(t *testing.T) {
	// Not parallel — installs global OTel providers.
	resetGlobals()

	withEnv(t,
		"OTEL_EXPORTER_OTLP_ENDPOINT", "http://127.0.0.1:14318",
		"OTEL_SDK_DISABLED", "",
	)

	sd := InitOTel(context.Background(), "test-service", nil)
	if sd == nil {
		t.Fatal("InitOTel returned nil shutdown")
	}

	// Call shutdown with an already-cancelled context; the SDK providers should
	// handle this gracefully (they may return an error, but must not panic).
	cancelledCtx, cancel := context.WithCancel(context.Background())
	cancel()
	// Intentionally ignoring the error — a cancelled-context shutdown of an
	// unreachable OTLP exporter returns an error, which is expected.
	_ = sd(cancelledCtx)
}

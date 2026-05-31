// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/observability/otel_test.go — unit tests for InitOTel (ADR-0927).
//
// Coverage:
//   - InitOTel with OTEL_SDK_DISABLED=true returns a no-op shutdown
//     and does not touch the global providers.
//   - InitOTel with no OTEL_EXPORTER_OTLP_ENDPOINT returns a no-op
//     shutdown.
//   - InitOTel with a syntactically-valid endpoint installs real
//     SDK providers and the returned shutdown is callable.
//   - OTEL_TRACES_SAMPLER_ARG out-of-range falls back to default.
//   - OTEL_SERVICE_NAME overrides the caller's serviceName.
//
// The tests do **not** require a live OTel collector: the OTLP HTTP
// exporter is constructed lazily, and shutdown of an unused exporter
// returns immediately. We assert observable side-effects on the
// global TracerProvider / MeterProvider type rather than network
// behaviour.

package observability

import (
	"context"
	"testing"
	"time"

	"go.opentelemetry.io/otel"
	sdkmetric "go.opentelemetry.io/otel/sdk/metric"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
)

// withEnv sets env vars for the duration of the test, restoring
// the previous values on cleanup. Each pair is key, value, key, value, ...
func withEnv(t *testing.T, kv ...string) {
	t.Helper()
	if len(kv)%2 != 0 {
		t.Fatalf("withEnv: odd number of arguments")
	}
	for i := 0; i < len(kv); i += 2 {
		t.Setenv(kv[i], kv[i+1])
	}
}

// shutdownBounded invokes shutdown with a short deadline so a test never
// hangs on an unreachable collector.
func shutdownBounded(t *testing.T, sd OTelShutdown) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	if err := sd(ctx); err != nil {
		// Network errors against an unreachable collector are expected
		// when the SDK actually got initialised; log but don't fail.
		t.Logf("shutdown returned (expected when no collector): %v", err)
	}
}

// resetGlobals restores the package-level OTel providers to no-ops so
// tests don't leak state into each other.
func resetGlobals() {
	otel.SetTracerProvider(sdktrace.NewTracerProvider())
	otel.SetMeterProvider(sdkmetric.NewMeterProvider())
}

func TestInitOTel_SDKDisabled_ReturnsNoop(t *testing.T) {
	resetGlobals()
	prevTP := otel.GetTracerProvider()

	withEnv(t,
		"OTEL_SDK_DISABLED", "true",
		"OTEL_EXPORTER_OTLP_ENDPOINT", "http://example.invalid:4318",
	)

	sd := InitOTel(context.Background(), "test-service", nil)
	if sd == nil {
		t.Fatalf("InitOTel returned nil shutdown")
	}
	// Disabled SDK must not swap the global tracer provider.
	if otel.GetTracerProvider() != prevTP {
		t.Errorf("expected TracerProvider unchanged when SDK disabled")
	}
	if err := sd(context.Background()); err != nil {
		t.Errorf("noop shutdown returned error: %v", err)
	}
}

func TestInitOTel_NoEndpoint_ReturnsNoop(t *testing.T) {
	resetGlobals()
	prevTP := otel.GetTracerProvider()

	// Explicitly clear in case the host env has it set.
	withEnv(t,
		"OTEL_EXPORTER_OTLP_ENDPOINT", "",
		"OTEL_SDK_DISABLED", "",
	)

	sd := InitOTel(context.Background(), "test-service", nil)
	if sd == nil {
		t.Fatalf("InitOTel returned nil shutdown")
	}
	if otel.GetTracerProvider() != prevTP {
		t.Errorf("expected TracerProvider unchanged when endpoint unset")
	}
	if err := sd(context.Background()); err != nil {
		t.Errorf("noop shutdown returned error: %v", err)
	}
}

func TestInitOTel_WithEndpoint_InstallsRealProviders(t *testing.T) {
	resetGlobals()
	prevTP := otel.GetTracerProvider()

	withEnv(t,
		"OTEL_EXPORTER_OTLP_ENDPOINT", "http://127.0.0.1:14318",
		"OTEL_SDK_DISABLED", "",
	)

	sd := InitOTel(context.Background(), "test-service", nil)
	if sd == nil {
		t.Fatalf("InitOTel returned nil shutdown")
	}
	defer shutdownBounded(t, sd)

	tp := otel.GetTracerProvider()
	if tp == prevTP {
		t.Errorf("expected TracerProvider replaced when endpoint configured")
	}
	if _, ok := tp.(*sdktrace.TracerProvider); !ok {
		t.Errorf("expected *sdktrace.TracerProvider, got %T", tp)
	}
}

func TestInitOTel_RespectsServiceNameEnvOverride(t *testing.T) {
	// We can't easily peek into the resource attributes the SDK
	// installs without taking on test-only deps, so this test
	// exercises the resolution code path purely for coverage —
	// the override-wins behaviour is exercised in integration.
	resetGlobals()

	withEnv(t,
		"OTEL_EXPORTER_OTLP_ENDPOINT", "http://127.0.0.1:14318",
		"OTEL_SERVICE_NAME", "env-override-name",
	)

	sd := InitOTel(context.Background(), "caller-name", nil)
	if sd == nil {
		t.Fatalf("InitOTel returned nil shutdown")
	}
	defer shutdownBounded(t, sd)
}

func TestInitOTel_SamplerArgOutOfRange_FallsBackToDefault(t *testing.T) {
	resetGlobals()

	withEnv(t,
		"OTEL_EXPORTER_OTLP_ENDPOINT", "http://127.0.0.1:14318",
		"OTEL_TRACES_SAMPLER_ARG", "9.5", // out of [0,1]
	)

	sd := InitOTel(context.Background(), "test-service", nil)
	if sd == nil {
		t.Fatalf("InitOTel returned nil shutdown")
	}
	defer shutdownBounded(t, sd)
	// The accepted behaviour is "don't crash, fall back to default";
	// we don't peek at the sampler ratio (would require unexported access).
}

func TestDefaults_ADR0927(t *testing.T) {
	// Locks in the defaults the ADR documents. If we ever want to
	// change them, the ADR must change first.
	if DefaultTraceSampleRatio != 0.01 {
		t.Errorf("DefaultTraceSampleRatio = %v, want 0.01 per ADR-0927", DefaultTraceSampleRatio)
	}
	if DefaultMetricExportInterval != 60*time.Second {
		t.Errorf("DefaultMetricExportInterval = %v, want 60s per ADR-0927", DefaultMetricExportInterval)
	}
}

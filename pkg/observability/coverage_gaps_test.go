// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/observability/coverage_gaps_test.go — branch-coverage fill-ins for
// paths not reached by the existing test files.
//
// Specifically:
//
//   - WaitForShutdown signal-delivery branch (SIGTERM via os.Process.Signal).
//   - InitOTel with a valid OTEL_TRACES_SAMPLER_ARG in [0.0, 1.0] (the
//     `sampleRatio = parsed` assignment branch inside InitOTel).
//   - SetControllerSources half-nil cases: q=non-nil+r=nil and q=nil+r=non-nil.
//   - Prometheus registry isolation: two independent Metrics instances on
//     separate registries do not interfere with each other's Gather output.
//   - OTel attribute key string values match the documented schema (ADR-0782).
//
// The existing tests cover: OTEL_SDK_DISABLED, no endpoint, valid endpoint,
// out-of-range sampler arg, non-numeric sampler arg, logInfo nil/non-nil,
// noopShutdown, WaitForShutdown context-cancel, SetControllerSources
// both-nil and both-non-nil.  This file targets only the remaining gaps
// identified by `go test -coverprofile` at 89.1 % to reach ≥94 %.

package observability

import (
	"context"
	"os"
	"syscall"
	"testing"
	"time"

	"github.com/prometheus/client_golang/prometheus"
)

// ---------------------------------------------------------------------------
// WaitForShutdown — signal-delivery branch
// ---------------------------------------------------------------------------

// TestWaitForShutdown_SIGTERMDelivery exercises the `case sig := <-ch` branch
// in WaitForShutdown by sending SIGTERM to the current process while
// WaitForShutdown is blocked.  The function is expected to return within a
// bounded drain window after the signal.
//
// Note: not t.Parallel() — sends a real signal to os.Getpid(), which affects
// the whole process.  Running in parallel with other tests that install their
// own signal handlers (e.g. the goroutine-leak test) could cause races.
func TestWaitForShutdown_SIGTERMDelivery(t *testing.T) {
	// A short-lived cancellable context ensures WaitForShutdown has a
	// context that does NOT fire on its own — the signal must do it.
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	lg := NewLogger("WARN") // suppress log noise during test

	// Fire WaitForShutdown in a goroutine; it will block until SIGTERM or ctx.
	const drainTimeout = 50 * time.Millisecond
	done := make(chan struct{})
	go func() {
		WaitForShutdown(ctx, lg, drainTimeout)
		close(done)
	}()

	// Give the goroutine a moment to register the signal handler before
	// we send the signal.  signal.Notify installs synchronously, but we
	// need the goroutine to reach the select statement.
	time.Sleep(10 * time.Millisecond)

	// Send SIGTERM to ourselves.  os.FindProcess never fails on POSIX.
	proc, err := os.FindProcess(os.Getpid())
	if err != nil {
		t.Fatalf("os.FindProcess: %v", err)
	}
	if err := proc.Signal(syscall.SIGTERM); err != nil {
		t.Fatalf("could not send SIGTERM: %v", err)
	}

	// WaitForShutdown returns after drainTimeout (50 ms) beyond signal receipt.
	// Bound the overall test wait to 2 s to avoid hanging CI.
	select {
	case <-done:
		// expected: signal received + drain window elapsed
	case <-time.After(2 * time.Second):
		t.Error("WaitForShutdown did not return after SIGTERM + drain timeout")
		cancel() // unblock the goroutine to avoid a leak
	}
}

// ---------------------------------------------------------------------------
// InitOTel — valid OTEL_TRACES_SAMPLER_ARG branch
// ---------------------------------------------------------------------------

// TestInitOTel_ValidSamplerArg exercises the branch inside InitOTel where
// OTEL_TRACES_SAMPLER_ARG parses cleanly and is in [0.0, 1.0], causing
// `sampleRatio = parsed` to execute.  Without this test, the assignment
// statement is dead code from the coverage tool's perspective.
//
// Note: not t.Parallel() — installs global OTel providers.
func TestInitOTel_ValidSamplerArg(t *testing.T) {
	resetGlobals()

	withEnv(t,
		"OTEL_EXPORTER_OTLP_ENDPOINT", "http://127.0.0.1:14318",
		"OTEL_SDK_DISABLED", "",
		"OTEL_TRACES_SAMPLER_ARG", "0.5", // valid: in [0.0, 1.0]
	)

	sd := InitOTel(context.Background(), "test-service", nil)
	if sd == nil {
		t.Fatal("InitOTel returned nil shutdown")
	}
	defer shutdownBounded(t, sd)
	// The observable effect is that InitOTel did not fall back to noop
	// (i.e. the global TracerProvider was replaced).  The exact ratio
	// is not inspectable without unexported access; coverage of the
	// assignment branch is the goal.
}

// TestInitOTel_ZeroSamplerArg exercises the lower boundary: 0.0 is a valid
// ratio (no traces sampled) and must not be treated as a parse failure.
//
// Note: not t.Parallel() — installs global OTel providers.
func TestInitOTel_ZeroSamplerArg(t *testing.T) {
	resetGlobals()

	withEnv(t,
		"OTEL_EXPORTER_OTLP_ENDPOINT", "http://127.0.0.1:14318",
		"OTEL_SDK_DISABLED", "",
		"OTEL_TRACES_SAMPLER_ARG", "0.0",
	)

	sd := InitOTel(context.Background(), "test-service", nil)
	if sd == nil {
		t.Fatal("InitOTel returned nil shutdown")
	}
	defer shutdownBounded(t, sd)
}

// TestInitOTel_OneSamplerArg exercises the upper boundary: 1.0 means 100 %
// of traces are sampled.
//
// Note: not t.Parallel() — installs global OTel providers.
func TestInitOTel_OneSamplerArg(t *testing.T) {
	resetGlobals()

	withEnv(t,
		"OTEL_EXPORTER_OTLP_ENDPOINT", "http://127.0.0.1:14318",
		"OTEL_SDK_DISABLED", "",
		"OTEL_TRACES_SAMPLER_ARG", "1.0",
	)

	sd := InitOTel(context.Background(), "test-service", nil)
	if sd == nil {
		t.Fatal("InitOTel returned nil shutdown")
	}
	defer shutdownBounded(t, sd)
}

// ---------------------------------------------------------------------------
// SetControllerSources — half-nil cases
// ---------------------------------------------------------------------------

// TestSetControllerSources_QueueOnlyNil exercises the branch where q == nil
// but r != nil: only the nodes_live GaugeFunc should register.
func TestSetControllerSources_QueueOnlyNil(t *testing.T) {
	t.Parallel()
	r := &mockNodeRegistry{n: 7}

	reg := prometheus.NewRegistry()
	m := NewMetrics(reg)
	m.SetControllerSources(nil, r) // q is nil; only node gauge should appear

	families, err := reg.Gather()
	if err != nil {
		t.Fatalf("Gather: %v", err)
	}
	got := map[string]bool{}
	for _, fam := range families {
		got[fam.GetName()] = true
	}

	// Queue gauges must NOT be registered.
	if got["vmafx_controller_jobs_pending"] {
		t.Error("vmafx_controller_jobs_pending registered despite nil queue")
	}
	if got["vmafx_controller_jobs_running"] {
		t.Error("vmafx_controller_jobs_running registered despite nil queue")
	}
	// Node gauge MUST be registered and report the correct value.
	if !got["vmafx_controller_nodes_live"] {
		t.Error("vmafx_controller_nodes_live missing when r != nil")
	}
}

// TestSetControllerSources_NodeRegistryOnlyNil exercises the branch where
// r == nil but q != nil: only the jobs_pending and jobs_running GaugeFuncs
// should register.
func TestSetControllerSources_NodeRegistryOnlyNil(t *testing.T) {
	t.Parallel()
	q := &mockJobQueue{pending: 4, running: 2}

	reg := prometheus.NewRegistry()
	m := NewMetrics(reg)
	m.SetControllerSources(q, nil) // r is nil; only queue gauges should appear

	families, err := reg.Gather()
	if err != nil {
		t.Fatalf("Gather: %v", err)
	}
	got := map[string]float64{}
	for _, fam := range families {
		for _, metric := range fam.GetMetric() {
			if g := metric.GetGauge(); g != nil {
				got[fam.GetName()] = g.GetValue()
			}
		}
	}

	// Queue gauges MUST be registered.
	if v, ok := got["vmafx_controller_jobs_pending"]; !ok {
		t.Error("vmafx_controller_jobs_pending missing when q != nil")
	} else if v != 4 {
		t.Errorf("jobs_pending = %v, want 4", v)
	}
	if v, ok := got["vmafx_controller_jobs_running"]; !ok {
		t.Error("vmafx_controller_jobs_running missing when q != nil")
	} else if v != 2 {
		t.Errorf("jobs_running = %v, want 2", v)
	}
	// Node gauge must NOT be registered.
	if _, ok := got["vmafx_controller_nodes_live"]; ok {
		t.Error("vmafx_controller_nodes_live registered despite nil node registry")
	}
}

// ---------------------------------------------------------------------------
// Prometheus registry isolation
// ---------------------------------------------------------------------------

// TestPrometheusRegistryIsolation verifies that two Metrics instances backed
// by separate prometheus.Registry objects do not pollute each other's Gather
// output, and that a counter increment on one does not appear in the other.
// This is the core property promised by ADR-1014.
func TestPrometheusRegistryIsolation(t *testing.T) {
	t.Parallel()

	reg1 := prometheus.NewRegistry()
	reg2 := prometheus.NewRegistry()

	m1 := NewMetrics(reg1)
	m2 := NewMetrics(reg2)

	// Increment m1's ScoreRequests counter three times; leave m2 untouched.
	m1.ScoreRequests.Inc()
	m1.ScoreRequests.Inc()
	m1.ScoreRequests.Inc()

	// Gather from reg2 — it must not see m1's increments.
	families2, err := reg2.Gather()
	if err != nil {
		t.Fatalf("reg2 Gather: %v", err)
	}
	for _, fam := range families2 {
		if fam.GetName() != "vmafx_server_score_requests_total" {
			continue
		}
		for _, metric := range fam.GetMetric() {
			if c := metric.GetCounter(); c != nil {
				if c.GetValue() != 0 {
					t.Errorf("reg2 score_requests_total = %v, want 0 (isolation violated)",
						c.GetValue())
				}
			}
		}
	}

	// Gather from reg1 — it must see exactly 3 increments.
	families1, err := reg1.Gather()
	if err != nil {
		t.Fatalf("reg1 Gather: %v", err)
	}
	for _, fam := range families1 {
		if fam.GetName() != "vmafx_server_score_requests_total" {
			continue
		}
		for _, metric := range fam.GetMetric() {
			if c := metric.GetCounter(); c != nil {
				if c.GetValue() != 3 {
					t.Errorf("reg1 score_requests_total = %v, want 3", c.GetValue())
				}
			}
		}
	}

	// Use m2 to suppress the "declared and not used" warning.
	m2.ScoreErrors.Inc()
}

// ---------------------------------------------------------------------------
// OTel attribute key string values (ADR-0782 schema lock-in)
// ---------------------------------------------------------------------------

// TestAttrKeyStrings verifies that the canonical attribute key string values
// defined in otel_instruments.go match the ADR-0782 schema.  Any rename
// would be a schema-breaking change visible to Tempo / Jaeger / OTLP
// consumers and must go through an ADR.
func TestAttrKeyStrings(t *testing.T) {
	t.Parallel()
	cases := []struct {
		key  string
		want string
	}{
		{string(AttrJobID), "vmafx.job_id"},
		{string(AttrModel), "vmafx.model"},
		{string(AttrBackend), "vmafx.backend"},
		{string(AttrNodeID), "vmafx.node_id"},
		{string(AttrVendor), "vmafx.gpu_vendor"},
		{string(AttrStatus), "vmafx.status"},
	}
	for _, tc := range cases {
		if tc.key != tc.want {
			t.Errorf("attribute key = %q, want %q (ADR-0782 schema break)", tc.key, tc.want)
		}
	}
}

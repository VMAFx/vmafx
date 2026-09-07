// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// bootstrap_test.go — the OpenTelemetry contract every vmafx binary inherits
// from Base (ADR-0782, ADR-1119):
//
//   - with no OTLP endpoint configured, otel.Module yields no-op providers
//     (the "tracing disabled by default" behaviour deployments rely on);
//   - withServiceIdentity completes otel.Options with service.version from
//     pkg/version and honours OTEL_SERVICE_NAME behind the vmafx config key,
//     and — being a root-scope decorator — reaches golusoris's own module;
//   - TraceHTTPHandler names spans "<METHOD> <path>", filters probe / scrape
//     endpoints, and HTTPTracing applies it to the handler golusoris.HTTP
//     serves.
//
// None of these tests use t.Parallel(): they install the process-global
// TracerProvider (via oteltest.Recorder) or mutate the environment.

package bootstrap

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/go-chi/chi/v5"
	"github.com/golusoris/golusoris"
	"github.com/golusoris/golusoris/config"
	"github.com/golusoris/golusoris/otel"
	"go.uber.org/fx"
	"go.uber.org/fx/fxtest"

	"github.com/VMAFx/vmafx/internal/oteltest"
	"github.com/VMAFx/vmafx/pkg/version"
)

// testEnvOptions mirrors the VMAFX_ env contract every binary installs with
// fx.Replace ahead of Base (ADR-1119 §2), minus the file watcher.
func testEnvOptions() config.Options {
	return config.Options{EnvPrefix: "VMAFX_", Delimiter: ".", Watch: false}
}

// quietEnv pins the no-op OTel path and a quiet logger, and clears every
// identity override so each test starts from the derived defaults.
func quietEnv(t *testing.T) {
	t.Helper()
	oteltest.NoopEnv(t)
	t.Setenv("VMAFX_LOG_LEVEL", "error")
	t.Setenv("OTEL_SERVICE_NAME", "")
	t.Setenv("VMAFX_OTEL_SERVICE_NAME", "")
	t.Setenv("VMAFX_OTEL_SERVICE_VERSION", "")
}

// buildOptions constructs the Base graph (no server module) and returns the
// otel.Options golusoris's module actually consumed, i.e. after decoration.
func buildOptions(t *testing.T) (otel.Options, *otel.Providers) {
	t.Helper()
	var (
		opts      otel.Options
		providers *otel.Providers
	)
	app := fx.New(
		Base,
		fx.Replace(testEnvOptions()),
		fx.NopLogger,
		fx.Populate(&opts, &providers),
	)
	if err := app.Err(); err != nil {
		t.Fatalf("Base graph failed to build: %v", err)
	}
	return opts, providers
}

func TestBase_OTelIsNoopWithoutEndpoint(t *testing.T) {
	quietEnv(t)
	opts, providers := buildOptions(t)

	if providers == nil {
		t.Fatal("otel.Module did not provide *otel.Providers")
	}
	if providers.Tracer != nil || providers.Meter != nil || providers.Logger != nil {
		t.Fatalf("expected no-op providers without an OTLP endpoint, got %+v", providers)
	}
	if !opts.Enabled {
		t.Errorf("otel.Options.Enabled = false; golusoris default is true (endpoint-gated no-op)")
	}
	// golusoris derives service.name from the build-info path: for a test
	// binary that is "<import path>.test", so this package yields "bootstrap";
	// for a shipped binary it is the cmd/ directory name (vmafx-server, ...).
	if opts.Service.Name != "bootstrap" {
		t.Errorf("derived service.name = %q, want %q", opts.Service.Name, "bootstrap")
	}
	if opts.Service.Version != version.Version() {
		t.Errorf("service.version = %q, want pkg/version %q", opts.Service.Version, version.Version())
	}
}

func TestBase_ServiceIdentityPrecedence(t *testing.T) {
	cases := []struct {
		name        string
		env         map[string]string
		wantName    string
		wantVersion string
	}{
		{
			name:        "OTEL_SERVICE_NAME overrides the derived name",
			env:         map[string]string{"OTEL_SERVICE_NAME": "svc-from-otel-env"},
			wantName:    "svc-from-otel-env",
			wantVersion: version.Version(),
		},
		{
			name: "VMAFX_OTEL_SERVICE_NAME beats OTEL_SERVICE_NAME",
			env: map[string]string{
				"OTEL_SERVICE_NAME":       "svc-from-otel-env",
				"VMAFX_OTEL_SERVICE_NAME": "svc-from-vmafx-cfg",
			},
			wantName:    "svc-from-vmafx-cfg",
			wantVersion: version.Version(),
		},
		{
			name:        "VMAFX_OTEL_SERVICE_VERSION beats pkg/version",
			env:         map[string]string{"VMAFX_OTEL_SERVICE_VERSION": "9.9.9-pinned"},
			wantName:    "bootstrap",
			wantVersion: "9.9.9-pinned",
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			quietEnv(t)
			for k, v := range tc.env {
				t.Setenv(k, v)
			}
			opts, _ := buildOptions(t)
			if opts.Service.Name != tc.wantName {
				t.Errorf("service.name = %q, want %q", opts.Service.Name, tc.wantName)
			}
			if opts.Service.Version != tc.wantVersion {
				t.Errorf("service.version = %q, want %q", opts.Service.Version, tc.wantVersion)
			}
		})
	}
}

func TestWithServiceIdentity_KeepsExplicitValues(t *testing.T) {
	quietEnv(t)
	t.Setenv("OTEL_SERVICE_NAME", "ignored-when-config-set")
	t.Setenv("VMAFX_OTEL_SERVICE_NAME", "explicit")
	cfg, err := config.New(testEnvOptions())
	if err != nil {
		t.Fatalf("config.New: %v", err)
	}
	in := otel.Options{Service: otel.ServiceOptions{Name: "explicit", Version: "1.2.3"}}
	got := withServiceIdentity(in, cfg, version.Info{Version: "from-build"})
	if got.Service.Name != "explicit" {
		t.Errorf("explicit config name replaced: %q", got.Service.Name)
	}
	if got.Service.Version != "1.2.3" {
		t.Errorf("explicit version replaced: %q", got.Service.Version)
	}
}

func TestTraceHTTPHandler_SpanNamesAndFilters(t *testing.T) {
	sr := oteltest.Recorder(t)
	h := TraceHTTPHandler(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	}))

	serve := func(path string) {
		t.Helper()
		rec := httptest.NewRecorder()
		h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, path, nil))
		if rec.Code != http.StatusNoContent {
			t.Fatalf("GET %s: status %d, handler chain broken", path, rec.Code)
		}
	}

	serve("/v1/score")
	if got := oteltest.Ended(sr, "GET /v1/score"); len(got) != 1 {
		t.Fatalf("GET /v1/score: want exactly one span, got %v", oteltest.Names(sr))
	}
	serve("/swagger/index.html")
	if got := oteltest.Ended(sr, "GET /swagger/*"); len(got) != 1 {
		t.Errorf("swagger subtree must collapse to one span name, got %v", oteltest.Names(sr))
	}
	for _, probe := range []string{"/healthz", "/readyz", "/livez", "/startupz", "/metrics"} {
		serve(probe)
	}
	if n := len(sr.Ended()); n != 2 {
		t.Errorf("probe/scrape endpoints must not be traced: %d spans %v", n, oteltest.Names(sr))
	}
}

func TestHTTPTracing_DecoratesGolusorisServerHandler(t *testing.T) {
	quietEnv(t)
	t.Setenv("VMAFX_HTTP_ADDR", "127.0.0.1:0")
	sr := oteltest.Recorder(t)

	var srv *http.Server
	app := fxtest.New(t,
		Base,
		fx.Replace(testEnvOptions()),
		fx.NopLogger,
		golusoris.HTTP,
		HTTPTracing,
		fx.Invoke(func(r chi.Router) {
			r.Get("/v1/health", func(w http.ResponseWriter, _ *http.Request) {
				w.WriteHeader(http.StatusOK)
			})
		}),
		fx.Populate(&srv),
	)
	app.RequireStart()
	defer app.RequireStop()

	rec := httptest.NewRecorder()
	srv.Handler.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/v1/health", nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("GET /v1/health through the golusoris server handler: status %d", rec.Code)
	}
	if got := oteltest.Ended(sr, "GET /v1/health"); len(got) != 1 {
		t.Fatalf("HTTPTracing did not reach golusoris.HTTP's handler; spans: %v", oteltest.Names(sr))
	}
}

// TestBase_ShutdownFlushesProviders proves the fx OnStop hook golusoris
// registers for the providers runs on app.Stop even in no-op mode (Shutdown on
// empty Providers is nil-safe), so every binary's exit path stays graceful.
func TestBase_ShutdownFlushesProviders(t *testing.T) {
	quietEnv(t)
	var providers *otel.Providers
	app := fxtest.New(t, Base, fx.Replace(testEnvOptions()), fx.NopLogger, fx.Populate(&providers))
	app.RequireStart()
	app.RequireStop()
	if err := providers.Shutdown(context.Background()); err != nil {
		t.Errorf("Shutdown on no-op providers returned %v", err)
	}
}

// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package bootstrap centralises the golusoris fx composition shared by every
// vmafx binary (ADR-1119). Each binary's main() starts from [Base] and adds
// its own server modules (golusoris.HTTP / grpc.Module / k8s/operator) plus
// its domain providers. Wiring the common stanza here keeps the composition
// root identical across cmd/vmafx-{server,controller,node,operator,mcp,tune}.
//
// OpenTelemetry (ADR-0782, ADR-1119) enters every binary through this package
// and nowhere else: [Base] carries golusoris's otel.Module (OTLP/gRPC exporter,
// W3C TraceContext + Baggage propagators, fx OnStop flush, silent no-op when no
// OTLP endpoint is configured) and [withServiceIdentity] completes the resource
// identity with the build version from pkg/version. HTTP surfaces add
// [HTTPTracing]; a hand-rolled http.Server wraps its handler with
// [TraceHTTPHandler]. Operator guide: docs/development/observability.md.
package bootstrap

import (
	"log/slog"
	"net/http"
	"os"
	"strings"

	"github.com/golusoris/golusoris"
	"github.com/golusoris/golusoris/config"
	"github.com/golusoris/golusoris/otel"
	"go.opentelemetry.io/contrib/instrumentation/net/http/otelhttp"
	"go.uber.org/fx"
	"go.uber.org/fx/fxevent"

	"github.com/VMAFx/vmafx/pkg/version"
)

// otelServiceNameEnv is the OTel-standard service.name override. golusoris's
// otel.Module reads only its own config key (otel.service.name, i.e.
// VMAFX_OTEL_SERVICE_NAME) and otherwise derives the name from the binary, so
// without withServiceIdentity the standard variable would be silently ignored.
const otelServiceNameEnv = "OTEL_SERVICE_NAME"

// otelServiceNameKey is the koanf path golusoris's otel.Module unmarshals
// service.name from (VMAFX_OTEL_SERVICE_NAME under the vmafx env prefix).
const otelServiceNameKey = "otel.service.name"

// Base is the module set every vmafx service shares:
//
//   - golusoris.Core — config (koanf; binaries override the env prefix to
//     "VMAFX_" via fx.Replace(config.Options{...}) per ADR-1119), structured
//     slog logging, clock, id, validate, crypto.
//   - otel.Module — OpenTelemetry tracer/meter/logger over OTLP/gRPC; a silent
//     no-op when no exporter endpoint is configured (ADR-0782 "best-effort and
//     non-blocking"). Its otel.Options are completed by withServiceIdentity.
//   - the build version (pkg/version.Info), supplied into the graph. golusoris
//     shipped its own version module in v0.5.0 (golusoris.Version, golusoris#226);
//     vmafx keeps this local Info because it also carries the VCS revision +
//     build metadata read from runtime/debug, beyond golusoris' ldflags string.
//
// It deliberately does NOT include a server module: a binary picks
// golusoris.HTTP, grpc.Module, and/or k8s/operator as appropriate. Binaries
// that wire golusoris.HTTP add [HTTPTracing] next to it.
var Base = fx.Options(
	golusoris.Core,
	otel.Module,
	fx.Supply(version.Get()),
	fx.Decorate(withServiceIdentity),
)

// withServiceIdentity is the root-scope fx decorator that completes golusoris's
// otel.Options with the vmafx resource identity (ADR-0782: service.name and
// service.version resource attributes on every binary):
//
//   - service.version comes from pkg/version when the operator did not pin
//     VMAFX_OTEL_SERVICE_VERSION. golusoris only reads the config key, so
//     without this the attribute would be absent from every span.
//   - service.name honours the OTel-standard OTEL_SERVICE_NAME when the vmafx
//     config key (VMAFX_OTEL_SERVICE_NAME) is unset. Otherwise the name golusoris
//     derived from the binary (vmafx-server, vmafx-node, ...) stands. Explicit
//     vmafx config wins over the standard env var, matching the precedence
//     golusoris applies to every other otel.* key.
//
// It is a decorator rather than a replacement provider so golusoris's own
// loadOptions (env/file parsing, derived default name) keeps running; the
// decorated value is what otel.Module's provider consumes. Root-scope decorators
// apply to child modules (the same pattern cmd/vmafx-node uses for
// grpcmod.Config), which internal/app/bootstrap's tests lock in.
func withServiceIdentity(o otel.Options, cfg *config.Config, v version.Info) otel.Options {
	if o.Service.Version == "" {
		o.Service.Version = v.Version
	}
	if cfg.Get(otelServiceNameKey) == "" {
		if name := os.Getenv(otelServiceNameEnv); name != "" {
			o.Service.Name = name
		}
	}
	return o
}

// HTTPTracing wraps the http.Handler golusoris.HTTP serves (the chi router
// provided by httpx/router.Module) in the upstream otelhttp server middleware
// via [TraceHTTPHandler], so every HTTP route gets a server span named
// "<METHOD> <path>" carrying the standard http.* attributes, parented to any
// inbound W3C traceparent. This closes the ADR-0782 follow-up ("wrap the HTTP
// mux with otelhttp.NewHandler") for the fx binaries. Add it to a composition
// root alongside golusoris.HTTP; it is a no-op on graphs without an
// http.Handler consumer and so is kept out of [Base] rather than forcing an
// unused decoration onto gRPC-only binaries.
var HTTPTracing = fx.Decorate(TraceHTTPHandler)

// httpTracingOperation is the otelhttp "operation" — the fallback span name
// and the instrumentation scope label — for every vmafx HTTP surface.
const httpTracingOperation = "vmafx.http"

// TraceHTTPHandler returns h wrapped in the otelhttp server middleware. It is
// the single HTTP instrumentation point for every vmafx binary: HTTPTracing
// applies it through fx, and hand-rolled servers (the vmafx-mcp streamable-HTTP
// transport) call it directly. Spans use the global TracerProvider that
// otel.Module installs, so the wrapper is inert (no spans, no allocation beyond
// the handler chain) when OTel is in no-op mode.
//
// Kubernetes probe and Prometheus scrape endpoints are filtered out: they fire
// every few seconds, carry no request-path information, and would dominate
// the trace volume at golusoris's default 100 % sample ratio.
func TraceHTTPHandler(h http.Handler) http.Handler {
	return otelhttp.NewHandler(h, httpTracingOperation,
		otelhttp.WithSpanNameFormatter(httpSpanName),
		otelhttp.WithFilter(traceHTTPRequest),
	)
}

// httpSpanName names HTTP server spans "<METHOD> <path>". The vmafx HTTP
// surfaces are fixed routes (/v1/score, /v1/health, /v1/ready, /swagger, ...),
// so the raw path is bounded-cardinality except for the Swagger UI subtree,
// which is collapsed to its wildcard.
func httpSpanName(_ string, r *http.Request) string {
	p := r.URL.Path
	if strings.HasPrefix(p, "/swagger/") {
		p = "/swagger/*"
	}
	return r.Method + " " + p
}

// traceHTTPRequest is the otelhttp filter: probe and scrape endpoints are not
// traced. Everything else is.
func traceHTTPRequest(r *http.Request) bool {
	switch r.URL.Path {
	case "/healthz", "/readyz", "/livez", "/startupz", "/metrics":
		return false
	default:
		return true
	}
}

// FxLogger routes fx's own lifecycle events (provide/invoke/start/stop) onto
// the golusoris *slog.Logger so dependency-graph and lifecycle events share
// the application log stream and its OTel correlation, instead of fx's default
// stderr printer. Add it to a binary's fx.New(...) alongside [Base].
func FxLogger() fx.Option {
	return fx.WithLogger(func(l *slog.Logger) fxevent.Logger {
		return &fxevent.SlogLogger{Logger: l}
	})
}

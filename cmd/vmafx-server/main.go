// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/main.go — vmafx-server entry point.
//
// Composition root: the server is wired as an fx application over the golusoris
// framework (ADR-1119 Phase-1 PR-1). golusoris owns config, structured logging,
// OpenTelemetry, the HTTP stack (chi router + graceful *http.Server), the gRPC
// server (OTel + logging + recovery interceptors baked in), and signal handling
// / graceful shutdown. This file supplies only the vmafx domain providers
// (scorer, metrics, concurrency limiter, gRPC service impl) and mounts the HTTP
// routes + health checks.
//
// Configuration (koanf via golusoris/config, env prefix VMAFX_, "." delimiter).
// The golusoris env transform strips the VMAFX_ prefix, lowercases, and replaces
// EVERY underscore with the "." delimiter, so the koanf key is the dotted form
// shown in the third column:
//
//	VMAFX_HTTP_ADDR              -> http.addr     HTTP listen address (default ":8080").
//	VMAFX_GRPC_LISTEN            -> grpc.listen   gRPC listen address (golusoris default ":9090").
//	VMAFX_LOG_LEVEL             -> log.level (golusoris v0.5.0 #234) slog level (default "INFO").
//	VMAFX_VMAF_BINARY            -> vmaf.binary   Path to the vmaf CLI binary (default: PATH lookup).
//	VMAFX_MODEL_DIR              -> model.dir     Directory containing VMAF .json model files.
//	VMAFX_MAX_CONCURRENT_SCORES  -> max.concurrent.scores  Max simultaneous Score calls (default: NumCPU).
//	VMAFX_SWAGGER_TRY_IT_OUT     Set to "1" to enable Swagger UI try-it-out.
//
// NOTE on the env-var contract change (ADR-1119): the pre-fx server used
// VMAFX_PORT / VMAFX_GRPC_PORT (bare port numbers). golusoris' httpx/server and
// grpc modules read the sub-keys http.addr and grpc.listen, which under the
// VMAFX_ prefix become VMAFX_HTTP_ADDR and VMAFX_GRPC_LISTEN and take a full
// listen address (":8080"), not a bare port. Operators must migrate. The
// golusoris-native defaults apply (http.addr ":8080", grpc.listen ":9090") —
// no legacy wire-default is carried.
//
// ADR-0703: vmafx-server Go gRPC + HTTP service (original hand-rolled root).
// ADR-0782: OpenTelemetry tracing and metrics.
// ADR-1119: golusoris fx framework adoption.

//go:build cgo

package main

import (
	"context"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"runtime"

	"github.com/go-chi/chi/v5"
	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promhttp"
	"go.uber.org/fx"
	googlegrpc "google.golang.org/grpc"

	"github.com/golusoris/golusoris"
	"github.com/golusoris/golusoris/core/clock"
	"github.com/golusoris/golusoris/core/config"
	grpcmod "github.com/golusoris/golusoris/grpc"
	"github.com/golusoris/golusoris/k8s/health"
	"github.com/golusoris/golusoris/observability/statuspage"

	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	"github.com/VMAFx/vmafx/internal/app/bootstrap"
	"github.com/VMAFx/vmafx/internal/app/scoringservice"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
	"github.com/VMAFx/vmafx/pkg/observability"
	buildversion "github.com/VMAFx/vmafx/pkg/version"
)

// version returns the shared build-time version string, falling back to "dev".
func version() string { return buildversion.Version() }

// isVersionRequest keeps the release-image smoke path independent of network
// listeners and the server's long-running fx lifecycle. The ELF loader still
// resolves the dynamically linked libvmaf before main runs.
func isVersionRequest(args []string) bool {
	return len(args) == 2 && args[1] == "--version"
}

// serverEnvOptions pins the VMAFX_ env contract for this binary. golusoris'
// grpc.Module reads four underscore-bearing leaf keys (grpc.cert_file,
// grpc.key_file, grpc.max_recv_size, grpc.max_send_size); the env transform
// splits EVERY underscore on the delimiter, so VMAFX_GRPC_MAX_RECV_SIZE would
// otherwise map to grpc.max.recv.size and silently fail to bind. Declaring each
// as a CompoundKey keeps its leaf underscores intact through the transform.
// watch is true for the binary (a mounted ConfigMap update triggers reload) and
// false for tests (no watcher goroutine).
func serverEnvOptions(watch bool) config.Options {
	return config.Options{
		EnvPrefix: "VMAFX_",
		Delimiter: ".",
		Watch:     watch,
		CompoundKeys: []string{
			"grpc.cert_file",
			"grpc.key_file",
			"grpc.max_recv_size",
			"grpc.max_send_size",
		},
	}
}

func main() {
	if isVersionRequest(os.Args) {
		fmt.Println(version())
		return
	}

	fx.New(productionOptions(fx.Replace(serverEnvOptions(true)))...).Run()
}

// productionOptions is shared by main and the lifecycle tests so the tested
// provider order cannot drift from the binary's real composition.
func productionOptions(envReplace fx.Option) []fx.Option {
	return []fx.Option{
		bootstrap.Base,
		envReplace,
		bootstrap.FxLogger(),
		golusoris.HTTP,
		bootstrap.HTTPTracing,
		grpcmod.Module,
		serverProviders(),
		fx.Invoke(realiseScorer),
		fx.Invoke(registerScoringService),
		fx.Invoke(mountHTTPRoutes),
		fx.Invoke(registerHealthChecks),
		fx.Invoke(realiseHTTPServer),
	}
}

func serverProviders() fx.Option {
	return fx.Provide(
		provideScorer,
		scoringservice.ProvideMetrics,
		provideScoreLimiter,
		provideStatusRegistry,
		newGRPCServerImpl,
	)
}

// realiseScorer preserves R1: its stop hook is appended before gRPC's hook,
// so reverse-order shutdown drains gRPC before closing the scorer.
func realiseScorer(_ *libvmaf.Scorer) {}

func registerScoringService(impl *grpcServer, s *googlegrpc.Server) {
	vmafxv1.RegisterVmafxScoringServer(s, impl)
}

// realiseHTTPServer forces the lazy listener after routes and health checks
// are mounted. Its stop hook is therefore first during reverse-order shutdown.
func realiseHTTPServer(_ *http.Server) {}

// provideScorer constructs the libvmaf cgo scorer from config and registers its
// Close as an OnStop hook. Because fx runs OnStop hooks in reverse of
// construction order, and the composition root forces the Scorer to be
// constructed before the gRPC server (see the R1 ordering invoke in main), this
// Close runs strictly AFTER the gRPC server's GracefulStop has drained
// in-flight Score calls.
//
// R1 is FORWARD-LOOKING: libvmaf.Scorer.Close() is currently a no-op (the
// scorer shells out to the vmaf subprocess and holds no live C handle), so the
// ordering does not presently prevent a use-after-free. It is in place so the
// cgo-lifetime invariant already holds the day Close() starts releasing a real
// C resource.
func provideScorer(lc fx.Lifecycle, cfg *config.Config, log *slog.Logger) (*libvmaf.Scorer, error) {
	return scoringservice.ProvideScorer(lc, cfg, log, version())
}

// provideScoreLimiter builds the shared concurrency cap. The cap is read from
// VMAFX_MAX_CONCURRENT_SCORES; 0 / unset falls back to runtime.NumCPU().
func provideScoreLimiter(cfg *config.Config) (*ScoreLimiter, error) {
	// golusoris env transform: VMAFX_MAX_CONCURRENT_SCORES -> strip prefix ->
	// lowercase + every '_' -> "." -> "max.concurrent.scores". R5-3 / MAXCONCURRENT.
	maxScores := cfg.Int("max.concurrent.scores")
	if maxScores < 1 {
		maxScores = runtime.NumCPU()
	}
	limiter, err := NewScoreLimiter(maxScores)
	if err != nil {
		return nil, fmt.Errorf("init concurrency limiter: %w", err)
	}
	return limiter, nil
}

// provideStatusRegistry builds the health-check registry that backs the k8s
// probe endpoints. It uses the golusoris clock so uptime + check timeouts share
// the framework clock.
func provideStatusRegistry(clk clock.Clock) *statuspage.Registry {
	return statuspage.NewRegistry(clk)
}

// newGRPCServerImpl builds the gRPC service implementation. It depends on the
// *libvmaf.Scorer (so fx constructs the scorer first — load-bearing for R1) and
// the shared limiter so HTTP and gRPC enforce one system-wide concurrency cap.
func newGRPCServerImpl(
	scorer *libvmaf.Scorer,
	metrics *observability.Metrics,
	limiter *ScoreLimiter,
	log *slog.Logger,
) *grpcServer {
	return newGRPCServerWithLimiter(scorer, metrics, log, limiter)
}

// mountHTTPRoutes registers the vmafx HTTP surface on the golusoris chi router:
// the legacy/probe endpoints, the Prometheus /metrics endpoint, the OpenAPI REST
// adapter, the Swagger UI, and the k8s health probes (/livez, /readyz,
// /startupz) backed by the status registry.
func mountHTTPRoutes(
	r chi.Router,
	scorer *libvmaf.Scorer,
	metrics *observability.Metrics,
	registry *prometheus.Registry,
	impl *grpcServer,
	limiter *ScoreLimiter,
	reg *statuspage.Registry,
	log *slog.Logger,
) {
	hs := newHTTPServerWithLimiter(scorer, metrics, registry, log, impl, limiter)

	// Legacy Kubernetes probe aliases — kept for backwards compatibility.
	r.Get("/healthz", hs.handleHealthz)
	r.Get("/readyz", hs.handleReadyz)

	// k8s canonical probes (/livez, /readyz, /startupz) backed by the registry.
	// NOTE: /readyz above is the legacy JSON alias; the canonical probe set is
	// mounted under /livez and /startupz to avoid clobbering it. golusoris
	// health.Mount also registers /readyz, so mount the canonical livez/startupz
	// explicitly and leave the legacy /readyz JSON contract intact.
	r.Get("/livez", health.LivezHandler(reg))
	r.Get("/startupz", health.StartupzHandler(reg))

	// Prometheus metrics (golusoris OTel is OTLP; the Prometheus path is ours).
	r.Handle("/metrics", promhttp.HandlerFor(registry, promhttp.HandlerOpts{}))

	// Legacy /v1/score — retained for clients predating the OpenAPI contract.
	r.Post("/v1/score", hs.handleScore)

	// OpenAPI REST adapter: /v1/health, /v1/ready (ADR-0797).
	if impl != nil {
		adapter := newRestAdapter(impl, log)
		r.Get("/v1/health", adapter.GetHealth)
		r.Get("/v1/ready", adapter.GetReady)
	}

	// Swagger UI at /swagger and /swagger/spec.json.
	registerSwaggerUIChi(r, log)
}

// registerHealthChecks registers the readiness check on the status registry:
// the server is ready once the scorer is usable.
func registerHealthChecks(reg *statuspage.Registry, scorer *libvmaf.Scorer) {
	reg.Register(statuspage.Check{
		Name: "scorer",
		Tags: []string{health.TagReadiness, health.TagStartup},
		Fn: func(_ context.Context) error {
			if scorer == nil {
				return fmt.Errorf("scorer not initialised")
			}
			return nil
		},
	})
}

// registerSwaggerUIChi mounts the Swagger UI handlers on a chi.Router. It reuses
// the same handler funcs as the legacy *http.ServeMux registration. Note that
// chi routing differs from net/http.ServeMux: ServeMux treats a trailing-slash
// pattern ("/swagger/") as a subtree match, but chi matches it exactly and 404s
// any deeper path. The "/swagger/*" wildcard restores subtree serving so
// /swagger/anything reaches the index handler as it did pre-migration.
func registerSwaggerUIChi(r chi.Router, log *slog.Logger) {
	r.Get("/swagger/spec.json", handleSwaggerSpec(log))
	r.Get("/swagger", handleSwaggerIndex(log))
	r.Handle("/swagger/*", http.HandlerFunc(handleSwaggerIndex(log)))
}

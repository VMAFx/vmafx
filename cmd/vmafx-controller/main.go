// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/main.go — vmafx-controller entry point.
//
// Composition root: the controller is wired as an fx application over the
// golusoris framework (ADR-1119 Phase-1 PR-2). golusoris owns config, structured
// logging, OpenTelemetry, the HTTP stack (chi router + graceful *http.Server),
// the gRPC server (OTel + logging + recovery interceptors baked in), and signal
// handling / graceful shutdown. This file supplies only the vmafx domain
// providers (scorer, metrics, job queue, node registry, scheduler, auth
// middleware, gRPC service impls) and mounts the HTTP routes.
//
// The controller is the cluster brain for the VMAFX distributed platform
// (ADR-0709, ADR-0711). It exposes:
//   - gRPC VmafxScoring service   — direct scoring
//   - gRPC VmafxController service — job queue + node API
//   - HTTP /healthz /readyz /metrics /v1/score
//
// DELIBERATE DESIGN (ADR-1119): the embedded modernc.org/sqlite job queue is
// KEPT — the controller is NOT migrated onto golusoris.Jobs (river/Postgres).
// A single-binary SQLite queue is the whole point of the controller's
// zero-dependency operability story; only its construction is moved into fx
// (with an OnStop Close hook).
//
// #269 (golusoris): the JWT auth interceptors are injected into the golusoris
// gRPC server via grpc.ProvideServerOptionFn — a constructor that receives the
// fx-constructed *auth.Middleware and returns a grpc.ServerOption — which feeds
// the group:"grpc.serveropts" group the grpc.Module consumes. Framework
// interceptors (OTel, logging, recovery) always run; the app-supplied auth
// interceptors are appended after them.
//
// Configuration (koanf via golusoris/config, env prefix VMAFX_, "." delimiter).
// The golusoris env transform strips the VMAFX_ prefix, lowercases, and replaces
// EVERY underscore with the "." delimiter, so the koanf key is the dotted form
// shown in the third column. Leaf keys whose own name contains an underscore
// (auth.tenant_claim, auth.roles_claim) are declared as CompoundKeys so the
// transform does not split them:
//
//	VMAFX_HTTP_ADDR              -> http.addr        HTTP listen address (golusoris default ":8080").
//	VMAFX_GRPC_LISTEN            -> grpc.listen      gRPC listen address (golusoris default ":9090").
//	VMAFX_LOG_LEVEL            -> log.level (golusoris v0.5.0 #234) slog level (default "INFO").
//	VMAFX_VMAF_BINARY           -> vmaf.binary       Path to the vmaf CLI binary (default: PATH lookup).
//	VMAFX_MODEL_DIR             -> model.dir         Directory containing VMAF .json model files.
//	VMAFX_DB_PATH               -> db.path           SQLite database path (default "vmafx-controller.db").
//	VMAFX_AUTH_DISABLED         -> auth.disabled     Disable JWT auth (dev/internal only).
//	VMAFX_JWKS_ENDPOINT         -> jwks.endpoint     JWKS endpoint URL (OIDC).
//	VMAFX_AUTH_ISSUER           -> auth.issuer       Expected JWT "iss" claim.
//	VMAFX_AUTH_AUDIENCE         -> auth.audience     Expected JWT "aud" claim (optional).
//	VMAFX_AUTH_TENANT_CLAIM     -> auth.tenant_claim Tenant-id claim field (CompoundKey; default "tid").
//	VMAFX_AUTH_ROLES_CLAIM      -> auth.roles_claim  Roles claim field (CompoundKey; default "vmafx_roles").
//
// NOTE on the env-var contract change (ADR-1119): the pre-fx controller used
// VMAFX_PORT / VMAFX_GRPC_PORT (bare port numbers). golusoris' httpx/server and
// grpc modules read http.addr and grpc.listen, which under the VMAFX_ prefix
// become VMAFX_HTTP_ADDR and VMAFX_GRPC_LISTEN and take a full listen address
// (":8080"), not a bare port. The golusoris-native defaults apply — no legacy
// wire-default is carried. Operators must migrate.
//
// ADR-0703: vmafx-server Go gRPC + HTTP service (origin).
// ADR-0711: vmafx-controller Phase 4b.1 scope expansion.
// ADR-0782: OpenTelemetry tracing and metrics.
// ADR-0794: multi-tenant JWT auth gateway.
// ADR-1119: golusoris fx framework adoption.

//go:build cgo

package main

import (
	"context"
	"fmt"
	"log/slog"
	"net/http"

	"github.com/go-chi/chi/v5"
	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/collectors"
	"github.com/prometheus/client_golang/prometheus/promhttp"
	"go.uber.org/fx"
	googlegrpc "google.golang.org/grpc"

	"github.com/golusoris/golusoris"
	"github.com/golusoris/golusoris/config"
	grpcmod "github.com/golusoris/golusoris/grpc"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/auth"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/nodes"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/queue"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/scheduler"
	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
	"github.com/VMAFx/vmafx/internal/app/bootstrap"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
	"github.com/VMAFx/vmafx/pkg/observability"
)

// buildVersion is a build-time version string, populated by ldflags:
// -ldflags "-X main.buildVersion=v1.2.3". Falls back to "dev".
var buildVersion = "dev"

// version returns the build-time version string, falling back to "dev".
func version() string { return buildVersion }

// controllerEnvOptions returns the config.Options that pin the VMAFX_ env
// contract for the controller. The two auth-claim leaf keys carry underscores
// in their own name (auth.tenant_claim, auth.roles_claim); declaring them as
// CompoundKeys keeps those underscores intact through golusoris' env transform
// (otherwise VMAFX_AUTH_TENANT_CLAIM would map to auth.tenant.claim).
func controllerEnvOptions() config.Options {
	return config.Options{
		EnvPrefix: "VMAFX_",
		Delimiter: ".",
		Watch:     true,
		CompoundKeys: []string{
			"auth.tenant_claim",
			"auth.roles_claim",
		},
	}
}

func main() {
	// golusoris v0.5.0 (#234) reads log.level / log.format from the prefixed
	// config tree directly, so VMAFX_LOG_LEVEL / VMAFX_LOG_FORMAT bind through
	// the same VMAFX_ prefix as every other key — no env bridge needed.
	fx.New(productionOptions(fx.Replace(controllerEnvOptions()))...).Run()
}

// productionOptions is the ordered fx option list the controller binary runs.
// It is shared with app_test.go's graph-validation + lifecycle tests so the
// tests exercise the exact composition main() runs (including the lazy-provider
// HTTP/gRPC bind guards in the same relative positions).
//
// The config env-options replace is passed in as envReplace so the binary and
// the tests can each supply their own config.Options (the tests disable
// file-watch). fx forbids replacing the same type twice, so factoring it out as
// a parameter — rather than embedding a fixed fx.Replace and overriding it —
// keeps the graph free of a double-decorate error.
func productionOptions(envReplace fx.Option) []fx.Option {
	return []fx.Option{
		// golusoris foundation: config + log + clock + id + validate + crypto,
		// the OTel module, and the build-version supply (ADR-1119).
		bootstrap.Base,
		// Override the env prefix so the whole graph reads VMAFX_* config keys,
		// keeping the underscore-bearing auth-claim leaves intact.
		envReplace,
		// Route fx lifecycle events onto the golusoris slog logger.
		bootstrap.FxLogger(),

		// Server modules.
		golusoris.HTTP, // chi *chi.Mux (as chi.Router) + graceful *http.Server.
		grpcmod.Module, // *grpc.Server with OTel + logging + recovery interceptors.

		// #269: inject the JWT auth interceptors into the golusoris gRPC server
		// via grpc.ProvideServerOptionFn — a constructor that receives the
		// fx-constructed *auth.Middleware and returns a grpc.ServerOption, fed
		// into the group:"grpc.serveropts" group. Framework interceptors (OTel,
		// logging, recovery) always run; these app options are appended after
		// them. The interceptors enforce tenant isolation on every RPC; when
		// auth is disabled the middleware injects a synthetic "dev" tenant so the
		// chain is uniform across deployments.
		grpcmod.ProvideServerOptionFn(func(mw *auth.Middleware) googlegrpc.ServerOption {
			return googlegrpc.ChainUnaryInterceptor(mw.GRPCUnaryInterceptor())
		}),
		grpcmod.ProvideServerOptionFn(func(mw *auth.Middleware) googlegrpc.ServerOption {
			return googlegrpc.ChainStreamInterceptor(mw.GRPCStreamInterceptor())
		}),

		// Domain providers.
		fx.Provide(
			provideScorer,       // (fx.Lifecycle, *config.Config, *slog.Logger) -> (*libvmaf.Scorer, error)
			provideMetrics,      // () -> (*prometheus.Registry, *observability.Metrics)
			provideJobQueue,     // (fx.Lifecycle, *config.Config, *slog.Logger) -> (queue.Queue, error) — SQLite, OnStop Close
			provideNodeRegistry, // (fx.Lifecycle, *slog.Logger) -> *nodes.Registry — reaper Start@OnStart / Close@OnStop
			provideScheduler,    // (queue.Queue, *nodes.Registry, *slog.Logger) -> *scheduler.Scheduler
			provideAuthMW,       // (*config.Config, *slog.Logger) -> (*auth.Middleware, error)
			newScoringServer,    // (*libvmaf.Scorer, *observability.Metrics, *slog.Logger) -> *scoringServer
			newControllerServer, // (queue.Queue, *nodes.Registry, *scheduler.Scheduler, *observability.Metrics, *slog.Logger) -> *controllerServer
		),

		// R1 (cgo lifetime + drain ordering): force the Scorer, job queue and node
		// registry to be constructed BEFORE the gRPC server. fx appends OnStop
		// hooks in construction order and runs them in REVERSE, so realising these
		// domain resources first (their Close / reaper-stop hooks appended in
		// provideScorer / provideJobQueue / provideNodeRegistry) before the gRPC
		// server (its GracefulStop hook appended by grpcmod) makes the order at
		// stop: gRPC GracefulStop (drains in-flight RPCs) → node-registry reaper
		// stop + queue Close → scorer Close. Without this guard the registration
		// invoke below would construct the *grpc.Server first (it is the invoke's
		// first arg), appending its GracefulStop hook ahead of the domain hooks and
		// inverting the drain order. See TestStopOrder in app_test.go.
		fx.Invoke(func(_ *libvmaf.Scorer, _ queue.Queue, _ *nodes.Registry) {}),

		// Register both gRPC services on the golusoris server.
		fx.Invoke(func(s *googlegrpc.Server, sc *scoringServer, ct *controllerServer) {
			vmafxv1.RegisterVmafxScoringServer(s, sc)
			controllerv1.RegisterVmafxControllerServer(s, ct)
		}),

		// Wire the controller metric sources (queue depth, node count) onto the
		// Prometheus registry and publish the auth Middleware for the gRPC auth
		// interceptors. The queue + registry are already constructed by the R1
		// guard above; this invoke additionally pulls in the auth Middleware.
		fx.Invoke(wireControllerSources),

		// Mount the controller HTTP surface (health, /metrics, /v1/score +auth)
		// on the golusoris chi router.
		fx.Invoke(mountControllerHTTP),

		// Lazy-provider guard: force construction of the golusoris *grpc.Server so
		// its OnStart listener binds. Placed AFTER the registration invoke so its
		// OnStop (GracefulStop) is appended last and fires first in the reverse
		// stop order.
		fx.Invoke(func(_ *googlegrpc.Server) {}),

		// Lazy-provider guard (the server migration's #1 BLOCKER lesson): fx
		// providers are lazy — nothing else consumes *http.Server, so without this
		// invoke the httpx/server listener never binds and the controller serves
		// gRPC only. app_test.go asserts the bound Addr is non-empty after start.
		fx.Invoke(func(_ *http.Server) {}),
	}
}

// ---------------------------------------------------------------------------
// Domain providers
// ---------------------------------------------------------------------------

// provideScorer constructs the libvmaf cgo scorer from config and registers its
// Close as an OnStop hook (R1: runs after the gRPC GracefulStop drains in-flight
// Score calls). See the R1 ordering invoke in productionOptions.
func provideScorer(lc fx.Lifecycle, cfg *config.Config, log *slog.Logger) (*libvmaf.Scorer, error) {
	binary := cfg.Get("vmaf.binary")
	// golusoris env transform: VMAFX_MODEL_DIR -> "model.dir" (every '_' becomes
	// the "." delimiter; the env var name itself is unchanged).
	modelDir := cfg.Get("model.dir")
	scorer, err := libvmaf.New(binary, modelDir)
	if err != nil {
		return nil, fmt.Errorf("init scorer: %w", err)
	}
	log.Info("scorer initialised",
		"version", version(),
		"vmaf_binary", binary,
		"model_dir", modelDir,
	)
	lc.Append(fx.Hook{
		OnStop: func(_ context.Context) error {
			log.Info("closing scorer (after gRPC drain)")
			scorer.Close()
			return nil
		},
	})
	return scorer, nil
}

// provideMetrics builds the isolated Prometheus registry and the vmafx metric
// instruments. golusoris OTel is OTLP, not a Prometheus registry, so the
// Prometheus exposition path is preserved here unchanged (mounted at /metrics by
// mountControllerHTTP).
func provideMetrics() (*prometheus.Registry, *observability.Metrics) {
	registry := prometheus.NewRegistry()
	// collectors.* replaces the deprecated prometheus.NewGoCollector /
	// NewProcessCollector — staticcheck SA1019.
	registry.MustRegister(collectors.NewGoCollector())
	registry.MustRegister(collectors.NewProcessCollector(collectors.ProcessCollectorOpts{}))
	metrics := observability.NewMetrics(registry)
	return registry, metrics
}

// provideJobQueue opens (or creates) the embedded modernc.org/sqlite job queue
// and registers its Close as an OnStop hook.
//
// DELIBERATE (ADR-1119): the controller keeps its single-binary SQLite queue
// rather than adopting golusoris.Jobs (river/Postgres). Only construction +
// teardown move into fx; the queue logic is untouched.
func provideJobQueue(lc fx.Lifecycle, cfg *config.Config, log *slog.Logger) (queue.Queue, error) {
	// golusoris env transform: VMAFX_DB_PATH -> "db.path".
	dbPath := cfg.Get("db.path")
	if dbPath == "" {
		dbPath = "vmafx-controller.db"
	}
	q, err := queue.New(dbPath, log)
	if err != nil {
		return nil, fmt.Errorf("open job queue: %w", err)
	}
	lc.Append(fx.Hook{
		OnStop: func(_ context.Context) error {
			log.Info("closing job queue")
			return q.Close()
		},
	})
	return q, nil
}

// provideNodeRegistry constructs the in-memory node registry and binds its
// reaper goroutine to the fx lifecycle: Start(ctx) launches the reaper in
// OnStart (no goroutine spawned at construction) and Close() — which stops and
// awaits the reaper — runs in OnStop. This eliminates the prior pattern where
// NewRegistry(ctx) spawned a goroutine bound to a caller ctx, and guarantees a
// leak-free shutdown (mirrors the vmafx-node FeedbackClient lifecycle).
func provideNodeRegistry(lc fx.Lifecycle, log *slog.Logger) *nodes.Registry {
	r := nodes.NewRegistry(log)
	lc.Append(fx.Hook{
		OnStart: func(ctx context.Context) error {
			r.Start(ctx)
			return nil
		},
		OnStop: func(_ context.Context) error {
			log.Info("stopping node registry reaper")
			r.Close()
			return nil
		},
	})
	return r
}

// provideScheduler builds the FIFO + capability-match scheduler over the queue
// and node registry. It holds no state of its own and needs no lifecycle hook.
func provideScheduler(q queue.Queue, r *nodes.Registry, log *slog.Logger) *scheduler.Scheduler {
	return scheduler.New(q, r, log)
}

// provideAuthMW builds the JWT bearer-token auth middleware from config
// (ADR-0794). When auth.disabled is true the middleware injects a synthetic
// "dev" tenant; otherwise jwks.endpoint + auth.issuer are required.
func provideAuthMW(cfg *config.Config, log *slog.Logger) (*auth.Middleware, error) {
	mw, err := auth.New(auth.Config{
		// golusoris env transform: VMAFX_JWKS_ENDPOINT -> "jwks.endpoint".
		JWKSEndpoint: cfg.Get("jwks.endpoint"),
		Issuer:       cfg.Get("auth.issuer"),
		Audience:     cfg.Get("auth.audience"),
		// CompoundKeys keep these leaf underscores intact (see controllerEnvOptions).
		TenantClaim: cfg.Get("auth.tenant_claim"),
		RolesClaim:  cfg.Get("auth.roles_claim"),
		Disabled:    cfg.Bool("auth.disabled"),
		Logger:      log,
	})
	if err != nil {
		return nil, fmt.Errorf("init auth middleware: %w", err)
	}
	return mw, nil
}

// ---------------------------------------------------------------------------
// Invokes
// ---------------------------------------------------------------------------

// wireControllerSources publishes the controller's queue + node-registry gauges
// on the Prometheus registry. It depends on the queue and registry so fx
// constructs both (appending their lifecycle hooks) before the listeners bind.
// (The auth Middleware no longer needs threading here: golusoris#269's
// ProvideServerOptionFn pulls it directly into the gRPC interceptor options.)
func wireControllerSources(
	metrics *observability.Metrics,
	q queue.Queue,
	r *nodes.Registry,
) {
	metrics.SetControllerSources(q, r)
}

// mountControllerHTTP registers the controller HTTP surface on the golusoris chi
// router: /healthz, /readyz, /metrics, and /v1/score (auth-gated). It reuses the
// existing httpServer handlers unchanged.
func mountControllerHTTP(
	router chi.Router,
	scorer *libvmaf.Scorer,
	metrics *observability.Metrics,
	registry *prometheus.Registry,
	mw *auth.Middleware,
	log *slog.Logger,
) {
	hs := newHTTPServer(scorer, metrics, registry, mw, log)

	router.Get("/healthz", hs.handleHealthz)
	router.Get("/readyz", hs.handleReadyz)
	router.Handle("/metrics", promhttp.HandlerFor(registry, promhttp.HandlerOpts{}))

	// /v1/score requires at least vmafx:writer (or vmafx:admin). When auth is
	// configured, wrap the handler with the JWT middleware + role gate.
	scoreHandler := http.HandlerFunc(hs.handleScore)
	if mw != nil {
		requireWriter := mw.RequireRole(auth.RoleWriter, auth.RoleAdmin)
		router.Handle("/v1/score", mw.HTTPHandler(requireWriter(scoreHandler)))
	} else {
		router.Handle("/v1/score", scoreHandler)
	}
}

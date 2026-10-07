// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/main.go — vmafx-node entry point.
//
// Composition root: the worker node is wired as an fx application over the
// golusoris framework (ADR-1119 Phase-1 PR-3). golusoris owns config, structured
// logging, OpenTelemetry, the gRPC server (OTel + logging + recovery
// interceptors baked in), and signal handling / graceful shutdown. This file
// supplies only the vmafx domain providers (encoder probe, libvmaf scorer,
// executor, sidecar feedback client, gRPC scoring handler) and mounts the node
// health surface.
//
// The node serves one gRPC service — VmafxScoring (Score, ScoreStream,
// Health) — and a small HTTP surface for operators: the Prometheus /metrics
// page and the statuspage probes /livez, /readyz and /startupz
// (mountNodeHTTP). The HTTP listener defaults to :9090, the chart's
// node.metricsPort, and moves with VMAFX_HTTP_ADDR.
//
// Configuration (koanf via golusoris/config, env prefix VMAFX_, "." delimiter).
// The golusoris env transform strips the VMAFX_ prefix, lowercases, and replaces
// EVERY underscore with the "." delimiter, so the koanf key is the dotted form
// shown in the third column:
//
//	VMAFX_GRPC_LISTEN     -> grpc.listen      gRPC listen address (node default ":50052").
//	VMAFX_HTTP_ADDR       -> http.addr        HTTP listen address of /metrics and the probes (node default ":9090").
//	VMAFX_LOG_LEVEL       -> log.level (golusoris v0.7.0)  slog level.
//	VMAFX_LOG_FORMAT      -> log.format       log handler (auto|tint|json).
//	VMAFX_FFMPEG_BIN      -> ffmpeg.bin       Path to the ffmpeg binary (default: PATH lookup).
//	VMAFX_VMAF_BINARY     -> vmaf.binary      Path to the vmaf CLI binary (default: FindBinary()).
//	VMAFX_MODEL_DIR       -> model.dir        Directory containing VMAF .json model files.
//	VMAFX_BACKEND         -> backend          Scoring backend label for the executor (default "cpu").
//	VMAFX_SIDECAR_SOCKET  -> sidecar.socket   Online-training sidecar Unix socket (default /tmp/vmafx-sidecar.sock).
//	VMAFX_CONTROLLER_*, VMAFX_NODE_ID, VMAFX_NODE_SLOTS
//	                      -> controller.* / node.*  Controller client (controller_config.go).
//	VMAFX_STORAGE_MODE, VMAFX_STORAGE_MOUNT_ROOT, VMAFX_RCLONE_BIN, VMAFX_RCLONE_CONFIG
//	                      -> storage.* / rclone.*   Source URIs of controller jobs (storage_config.go).
//
// NOTE on the env-var contract change (ADR-1119): the pre-fx node used
// VMAFX_NODE_ADDR (a bare listen address). golusoris' grpc.Module reads the
// sub-key grpc.listen, which under the VMAFX_ prefix becomes VMAFX_GRPC_LISTEN.
// golusoris' grpc.Module supplies grpc.listen; withNodeGRPCDefault preserves
// the node's historical :50052 when no file or environment override exists.
// Operators must migrate VMAFX_NODE_ADDR -> VMAFX_GRPC_LISTEN.
//
// With VMAFX_CONTROLLER_ADDR set the node also runs the controller client
// (controller_client.go): it registers with the controller, heartbeats, pulls
// jobs, scores them through the Executor and reports the results (ADR-0713).
//
// The eBPF descriptor tracker (cmd/vmafx-node/bpf) is a privileged, opt-in
// side path: VMAFX_EBPF_BYPASS starts it (ebpf_linux.go), and a host that
// cannot run it stops the node at startup.
//
// ADR-0713: vmafx-node Go worker binary (original hand-rolled root).
// ADR-0717: ffmpeg baked into the node image at /usr/local/bin/ffmpeg.
// ADR-0781: sidecar online training feedback channel.
// ADR-0782: OpenTelemetry tracing and metrics.
// ADR-0933: in-memory per-frame ScoreStream.
// ADR-1119: golusoris fx framework adoption.

//go:build cgo

package main

import (
	"context"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"slices"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promhttp"
	"go.uber.org/fx"
	googlegrpc "google.golang.org/grpc"

	"github.com/golusoris/golusoris/core/clock"
	"github.com/golusoris/golusoris/core/config"
	grpcmod "github.com/golusoris/golusoris/grpc"
	"github.com/golusoris/golusoris/httpx/server"
	"github.com/golusoris/golusoris/k8s/health"
	"github.com/golusoris/golusoris/observability/statuspage"

	"github.com/VMAFx/vmafx/cmd/vmafx-node/probe"
	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	"github.com/VMAFx/vmafx/internal/app/bootstrap"
	"github.com/VMAFx/vmafx/internal/app/scoringservice"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
	buildversion "github.com/VMAFx/vmafx/pkg/version"
)

// probeTimeout bounds the startup encoder probe so a hung ffmpeg binary cannot
// stall node startup indefinitely (carried over from the pre-fx root).
const probeTimeout = 30 * time.Second

// defaultNodeHTTPAddr is the node's metrics and probe listener: the chart's
// node.metricsPort (9090). The shared HTTP module defaults to :8080, the
// server's and controller's API port.
const defaultNodeHTTPAddr = ":9090"

// defaultNodeGRPCListen preserves the node's pre-golusoris public port. The
// shared grpc module defaults to :9090, which is correct for the other Go
// services but would silently move standalone node listeners away from the
// Docker EXPOSE, Helm Service, and documented node contract.
const defaultNodeGRPCListen = ":50052"

// isVersionRequest keeps the release-image smoke path independent of the fx
// graph and its long-running gRPC lifecycle.
func isVersionRequest(args []string) bool {
	return len(args) == 2 && args[1] == "--version"
}

// nodeEnvOptions pins the VMAFX_ env contract for this binary. golusoris'
// grpc.Module reads four underscore-bearing leaf keys (grpc.cert_file,
// grpc.key_file, grpc.max_recv_size, grpc.max_send_size); the env transform
// splits EVERY underscore on the delimiter, so VMAFX_GRPC_MAX_RECV_SIZE would
// otherwise map to grpc.max.recv.size and silently fail to bind. Declaring each
// as a CompoundKey keeps its leaf underscores intact. watch is true for the
// binary (mounted ConfigMap reload) and false for tests.
func nodeEnvOptions(watch bool) config.Options {
	return config.Options{
		EnvPrefix: "VMAFX_",
		Delimiter: ".",
		Watch:     watch,
		CompoundKeys: slices.Concat([]string{
			"grpc.cert_file",
			"grpc.key_file",
			"grpc.max_recv_size",
			"grpc.max_send_size",
		}, controllerConfigKeys, storageConfigKeys, ebpfConfigKeys),
	}
}

// withNodeGRPCDefault decorates the framework config after it has loaded file
// and environment overrides. Only an absent or empty grpc.listen receives the
// node-specific default; an explicit :9090 (or any other address) is retained.
func withNodeGRPCDefault(framework grpcmod.Config, raw *config.Config) grpcmod.Config {
	if raw.Get("grpc.listen") == "" {
		framework.Listen = defaultNodeGRPCListen
	}
	return framework
}

// withNodeHTTPDefault gives the HTTP listener the node's default address when
// http.addr is absent or empty; an explicit address is retained.
func withNodeHTTPDefault(framework server.Options, raw *config.Config) server.Options {
	if raw.Get("http.addr") == "" {
		framework.Addr = defaultNodeHTTPAddr
	}
	return framework
}

func main() {
	if isVersionRequest(os.Args) {
		fmt.Println(buildversion.Version())
		return
	}

	fx.New(
		nodeFoundationOptions(),
		nodeDomainOptions(),
		nodeLifecycleOptions(),
	).Run()
}

// nodeFoundationOptions wires the golusoris foundation modules and the gRPC server
// module the node serves on.
func nodeFoundationOptions() fx.Option {
	return fx.Options(
		// golusoris foundation: config + log + clock + id + validate + crypto,
		// the OTel module, and the build-version supply (ADR-1119).
		bootstrap.Base,
		// Override the env prefix so the whole graph reads VMAFX_* config keys,
		// keeping the underscore-bearing grpc.* leaves intact (nodeEnvOptions).
		fx.Replace(nodeEnvOptions(true)),
		// Route fx lifecycle events onto the golusoris slog logger.
		bootstrap.FxLogger(),

		nodeServerOptions(),
	)
}

// nodeServerOptions wires the node's two listeners: the gRPC server (OTel +
// logging + recovery interceptors baked in) and HTTP for /metrics and the
// probes (mountNodeHTTP), traced like the server's and controller's.
func nodeServerOptions() fx.Option {
	return fx.Options(
		grpcmod.Module,
		fx.Decorate(withNodeGRPCDefault),
		bootstrap.HTTP,
		bootstrap.HTTPTracing,
		fx.Decorate(withNodeHTTPDefault),
	)
}

// nodeDomainOptions provides the node's domain objects.
func nodeDomainOptions() fx.Option {
	return fx.Provide(
		provideEncoderInventory, // (fx.Lifecycle, *config.Config, *slog.Logger) -> *probe.Inventory (OnStart probe, NON-FATAL)
		provideScorer,           // (fx.Lifecycle, *config.Config, *slog.Logger) -> *libvmaf.Scorer (nil-tolerant)
		provideExecutor,         // (*libvmaf.Scorer, *config.Config, *slog.Logger) -> (*Executor, error); storage layer per storage_config.go
		provideFeedbackClient,   // (fx.Lifecycle, *config.Config, *slog.Logger) -> *FeedbackClient (drainer OnStart, Close+awaited OnStop)
		provideControllerClient, // (controllerClientParams) -> *controllerClient (nil without VMAFX_CONTROLLER_ADDR; start OnStart, drain OnStop)
		provideEBPFBypass,       // -> *ebpfBypass (nil unless VMAFX_EBPF_BYPASS; tracker Start OnStart, fail closed)
		provideStatusRegistry,   // (clock.Clock) -> *statuspage.Registry
		provideNodeRegistry,     // -> *prometheus.Registry (Go + process collectors)
		provideNodeMetrics,      // (*prometheus.Registry, *Executor) -> (*nodeMetrics, error)
		provideStreamMetrics,    // (*prometheus.Registry) -> (*scoringservice.StreamMetrics, error)
		newScoringHandler,       // (*libvmaf.Scorer, *probe.Inventory, *slog.Logger) -> *scoringHandler
	)
}

// nodeLifecycleOptions holds the invokes whose registration order is the node's shutdown
// contract (R-node). fx appends OnStop hooks in construction order and runs them in
// reverse, so the order these invokes appear in is what produces the stop sequence the
// comments below describe. Reordering them changes the shutdown, not just the wiring.
func nodeLifecycleOptions() fx.Option {
	return fx.Options(
		// R-node (cgo lifetime): force the Scorer to be constructed BEFORE the
		// golusoris *grpc.Server. fx appends OnStop hooks in construction order and
		// runs them in reverse, so realising the Scorer first (its Close hook
		// appended in provideScorer) before the gRPC server (its GracefulStop hook
		// appended by grpcmod) makes gRPC drain in-flight Score / ScoreStream calls
		// BEFORE the Scorer closes. libvmaf.Scorer.Close() is presently a no-op
		// (the scorer is subprocess-based and holds no live C handle), so this
		// ordering does not currently prevent a use-after-free — it is a
		// forward-looking guard for when Close() acquires a real cgo resource. This
		// invoke is registered ahead of the gRPC registration invoke so the
		// Scorer's hook lands first. See app_test.go.
		fx.Invoke(func(_ *libvmaf.Scorer) {}),

		// Lazy-provider guard for the lifecycle-bearing domain objects. Nothing in
		// the gRPC scoring path consumes the *FeedbackClient (it is fed by the
		// controller-job executor path) or the *Executor, so without this invoke
		// fx never constructs them — the feedback drainer would never start and the
		// executor would never exist. Forcing construction HERE — after the Scorer
		// guard above and BEFORE the gRPC server registration below — appends their
		// OnStop hooks between the scorer's and the gRPC server's, so fx's
		// reverse-order stop fires: gRPC GracefulStop → FeedbackClient drainer stop
		// → scorer Close (R-node). See TestStopOrderNode in app_test.go.
		//
		// The controller client is realised last (it consumes the Executor), so
		// its drain runs right after gRPC GracefulStop and before the
		// FeedbackClient and the scorer stop: jobs it is running finish and are
		// reported while the executor still exists. See
		// TestStopOrderNodeWithController in controller_graph_test.go. The eBPF
		// tracker (when enabled) is realised before the client, so it watches
		// before the first job is pulled and stops after the last one drained.
		fx.Invoke(func(_ *FeedbackClient, _ *Executor, _ *ebpfBypass, _ *controllerClient) {}),

		// Register the VmafxScoring service on the golusoris gRPC server. The arg
		// order (scorer-bearing handler first, then the server) also keeps the
		// Scorer ahead of the server in construction order, reinforcing R-node.
		fx.Invoke(registerScoringService),

		// Lazy-provider guard: fx providers are lazy, so without a consumer of
		// *grpc.Server the golusoris grpc.Module's OnStart listener never binds and
		// the node serves nothing. This standalone invoke forces construction so
		// the listener comes up on VMAFX_GRPC_LISTEN. Placed AFTER the registration
		// invoke so the server's OnStop is appended LAST and therefore fires FIRST
		// in fx's reverse-order stop: gRPC GracefulStop drains in-flight RPCs →
		// FeedbackClient drainer stops → scorer Close. See app_test.go.
		fx.Invoke(func(_ *googlegrpc.Server) {}),

		// Mount the node health surface (statuspage readiness check + log).
		fx.Invoke(mountNodeHealth),

		// Mount /metrics and the probes on the HTTP router, then bind the
		// listener (lazy-provider guard, as for gRPC). Its OnStop is appended
		// last, so it stops first; scraping during the drain is not needed.
		fx.Invoke(mountNodeHTTP),
		fx.Invoke(func(_ *http.Server) {}),
	)
}

// mountNodeHTTP serves the node's /metrics page from its own registry
// (ADR-1014) and the statuspage probes.
func mountNodeHTTP(router chi.Router, reg *prometheus.Registry, checks *statuspage.Registry) {
	router.Handle("/metrics", promhttp.HandlerFor(reg, promhttp.HandlerOpts{}))
	health.Mount(router, checks)
}

// registerScoringService registers the VmafxScoring service on the golusoris
// gRPC server, its handler recording ScoreStream sessions in streams.
func registerScoringService(s *googlegrpc.Server, h *scoringHandler, streams *scoringservice.StreamMetrics) {
	h.streams = streams
	vmafxv1.RegisterVmafxScoringServer(s, h)
}

// provideStatusRegistry builds the health-check registry that backs the node
// readiness surface. It uses the golusoris clock so uptime + check timeouts
// share the framework clock.
func provideStatusRegistry(clk clock.Clock) *statuspage.Registry {
	return statuspage.NewRegistry(clk)
}

// mountNodeHealth registers the node's readiness check on the status registry
// and logs the served health contract. The VmafxScoring Health RPC stays
// available even without a scorer; the statuspage check below records whether
// scoring is actually usable (scorer present), which /readyz (mountNodeHTTP)
// reports — without it, k8s would mark a scorer-less node ready and route
// un-servable Score RPCs to it.
//
// Consuming *grpc.Server here is not required for the listener to bind (the
// standalone lazy-provider guard invoke in main does that); it is taken so this
// invoke is ordered after the gRPC registration and reads as "health depends on
// the server being wired".
func mountNodeHealth(
	reg *statuspage.Registry,
	scorer *libvmaf.Scorer,
	inv *probe.Inventory,
	log *slog.Logger,
) {
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
	log.Info("node health surface mounted",
		"probe", "grpc:VmafxScoring/Health",
		"scoring_enabled", scorer != nil,
		"encoders_available", len(inv.Available),
	)
}

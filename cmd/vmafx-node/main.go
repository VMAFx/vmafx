// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
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
// The node serves a single gRPC service — VmafxScoring (Score, ScoreStream,
// Health). It is gRPC-only: there is no HTTP server in the node binary, so the
// canonical k8s probe is the gRPC Health RPC (golusoris.HTTP is intentionally
// NOT in the graph). mountNodeHealth wires a statuspage readiness check (scorer
// usable) for parity with the server's seam and so an HTTP livez/readyz can be
// mounted in one place the day golusoris.HTTP is added to the node.
//
// Configuration (koanf via golusoris/config, env prefix VMAFX_, "." delimiter).
// The golusoris env transform strips the VMAFX_ prefix, lowercases, and replaces
// EVERY underscore with the "." delimiter, so the koanf key is the dotted form
// shown in the third column:
//
//	VMAFX_GRPC_LISTEN     -> grpc.listen      gRPC listen address (node default ":50052").
//	VMAFX_LOG_LEVEL       -> log.level (golusoris v0.7.0)  slog level.
//	VMAFX_LOG_FORMAT      -> log.format       log handler (auto|tint|json).
//	VMAFX_FFMPEG_BIN      -> ffmpeg.bin       Path to the ffmpeg binary (default: PATH lookup).
//	VMAFX_VMAF_BINARY     -> vmaf.binary      Path to the vmaf CLI binary (default: FindBinary()).
//	VMAFX_MODEL_DIR       -> model.dir        Directory containing VMAF .json model files.
//	VMAFX_BACKEND         -> backend          Scoring backend label for the executor (default "cpu").
//	VMAFX_SIDECAR_SOCKET  -> sidecar.socket   Online-training sidecar Unix socket (default /tmp/vmafx-sidecar.sock).
//
// NOTE on the env-var contract change (ADR-1119): the pre-fx node used
// VMAFX_NODE_ADDR (a bare listen address). golusoris' grpc.Module reads the
// sub-key grpc.listen, which under the VMAFX_ prefix becomes VMAFX_GRPC_LISTEN.
// golusoris' grpc.Module supplies grpc.listen; withNodeGRPCDefault preserves
// the node's historical :50052 when no file or environment override exists.
// Operators must migrate VMAFX_NODE_ADDR -> VMAFX_GRPC_LISTEN.
//
// The eBPF rclone-bypass loader (cmd/vmafx-node/bpf) is unrelated to golusoris
// and remains a privileged, opt-in side path; it is not wired into this graph.
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
	"os"
	"time"

	"go.uber.org/fx"
	googlegrpc "google.golang.org/grpc"

	"github.com/golusoris/golusoris/clock"
	"github.com/golusoris/golusoris/config"
	grpcmod "github.com/golusoris/golusoris/grpc"
	"github.com/golusoris/golusoris/k8s/health"
	"github.com/golusoris/golusoris/observability/statuspage"

	"github.com/VMAFx/vmafx/cmd/vmafx-node/probe"
	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	"github.com/VMAFx/vmafx/internal/app/bootstrap"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
	buildversion "github.com/VMAFx/vmafx/pkg/version"
)

// probeTimeout bounds the startup encoder probe so a hung ffmpeg binary cannot
// stall node startup indefinitely (carried over from the pre-fx root).
const probeTimeout = 30 * time.Second

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
		CompoundKeys: []string{
			"grpc.cert_file",
			"grpc.key_file",
			"grpc.max_recv_size",
			"grpc.max_send_size",
		},
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

func main() {
	if isVersionRequest(os.Args) {
		fmt.Println(buildversion.Version())
		return
	}

	fx.New(
		// golusoris foundation: config + log + clock + id + validate + crypto,
		// the OTel module, and the build-version supply (ADR-1119).
		bootstrap.Base,
		// Override the env prefix so the whole graph reads VMAFX_* config keys,
		// keeping the underscore-bearing grpc.* leaves intact (nodeEnvOptions).
		fx.Replace(nodeEnvOptions(true)),
		// Route fx lifecycle events onto the golusoris slog logger.
		bootstrap.FxLogger(),

		// gRPC server with OTel + logging + recovery interceptors baked in.
		grpcmod.Module,
		fx.Decorate(withNodeGRPCDefault),

		// Domain providers.
		fx.Provide(
			provideEncoderInventory, // (fx.Lifecycle, *config.Config, *slog.Logger) -> *probe.Inventory (OnStart probe, NON-FATAL)
			provideScorer,           // (fx.Lifecycle, *config.Config, *slog.Logger) -> *libvmaf.Scorer (nil-tolerant)
			provideExecutor,         // (*libvmaf.Scorer, *config.Config, *slog.Logger) -> *Executor
			provideFeedbackClient,   // (fx.Lifecycle, *config.Config, *slog.Logger) -> *FeedbackClient (drainer OnStart, Close+awaited OnStop)
			provideStatusRegistry,   // (clock.Clock) -> *statuspage.Registry
			newScoringHandler,       // (*libvmaf.Scorer, *probe.Inventory, *slog.Logger) -> *scoringHandler
		),

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
		fx.Invoke(func(_ *FeedbackClient, _ *Executor) {}),

		// Register the VmafxScoring service on the golusoris gRPC server. The arg
		// order (scorer-bearing handler first, then the server) also keeps the
		// Scorer ahead of the server in construction order, reinforcing R-node.
		fx.Invoke(func(s *googlegrpc.Server, h *scoringHandler) {
			vmafxv1.RegisterVmafxScoringServer(s, h)
		}),

		// Lazy-provider guard: fx providers are lazy, so without a consumer of
		// *grpc.Server the golusoris grpc.Module's OnStart listener never binds and
		// the node serves nothing. This standalone invoke forces construction so
		// the listener comes up on VMAFX_GRPC_LISTEN. Placed AFTER the registration
		// invoke so the server's OnStop is appended LAST and therefore fires FIRST
		// in fx's reverse-order stop: gRPC GracefulStop drains in-flight RPCs →
		// FeedbackClient drainer stops → scorer Close. See app_test.go.
		fx.Invoke(func(_ *googlegrpc.Server) {}),

		// Mount the node health surface (statuspage readiness check + log). The
		// node's k8s probe is the gRPC Health RPC; this seam carries the readiness
		// state and is where HTTP livez/readyz mount once golusoris.HTTP is wired.
		fx.Invoke(mountNodeHealth),
	).Run()
}

// provideStatusRegistry builds the health-check registry that backs the node
// readiness surface. It uses the golusoris clock so uptime + check timeouts
// share the framework clock.
func provideStatusRegistry(clk clock.Clock) *statuspage.Registry {
	return statuspage.NewRegistry(clk)
}

// mountNodeHealth registers the node's readiness check on the status registry
// and logs the served health contract. The node is gRPC-only, so its canonical
// k8s probe is the VmafxScoring Health RPC (always available, even without a
// scorer). The statuspage check below records whether scoring is actually
// usable (scorer present) so an HTTP /readyz can expose it the day
// golusoris.HTTP joins the node graph — without that, k8s would mark a
// scorer-less node ready and route un-servable Score RPCs to it.
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

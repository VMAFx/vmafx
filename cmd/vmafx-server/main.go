// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/main.go — vmafx-server entry point.
//
// Starts both the HTTP and gRPC servers concurrently.  Signal handling and
// graceful shutdown are wired through pkg/observability.
//
// 12-factor (§III) environment variables:
//
//	VMAFX_PORT                HTTP listen port (default "8080").
//	VMAFX_GRPC_PORT           gRPC listen port (default "50051").
//	VMAFX_LOG_LEVEL           slog level string (default "INFO").
//	VMAFX_VMAF_BINARY         Path to the vmaf CLI binary.
//	VMAFX_MODEL_DIR           Directory containing VMAF .json model files.
//	VMAFX_SWAGGER_TRY_IT_OUT  Set to "1" to enable Swagger UI try-it-out (default: disabled).
//
// CLI flags mirror and override the environment variables.
//
// ADR-0703: vmafx-server Go gRPC + HTTP service.
// ADR-0782: OpenTelemetry tracing and metrics.

//go:build cgo

package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"os"
	"runtime"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/collectors"
	"golang.org/x/sync/errgroup"

	"github.com/VMAFx/vmafx/pkg/libvmaf"
	"github.com/VMAFx/vmafx/pkg/observability"
)

// envOr returns the environment variable value, or def if the variable is unset / empty.
func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func main() {
	// ---------------------------------------------------------------------------
	// Flags — override env vars when explicitly supplied.
	// ---------------------------------------------------------------------------
	// Override the default flag.Usage so that -help / --help exits 0, not 2.
	flag.Usage = func() {
		fmt.Fprintf(os.Stdout, "Usage of vmafx-server:\n")
		flag.CommandLine.SetOutput(os.Stdout)
		flag.PrintDefaults()
	}
	port := flag.String("port", envOr("VMAFX_PORT", "8080"), "HTTP listen port")
	grpcPort := flag.String("grpc-port", envOr("VMAFX_GRPC_PORT", "50051"), "gRPC listen port")
	logLevel := flag.String("log-level", envOr("VMAFX_LOG_LEVEL", "INFO"), "log level (DEBUG|INFO|WARN|ERROR)")
	vmafBinary := flag.String("vmaf-binary", envOr("VMAFX_VMAF_BINARY", ""), "path to vmaf CLI binary (default: PATH lookup)")
	modelDir := flag.String("model-dir", envOr("VMAFX_MODEL_DIR", ""), "directory containing VMAF .json model files")
	maxConcurrentScores := flag.Int("max-concurrent-scores", runtime.NumCPU(),
		"maximum number of simultaneous Scorer.Score calls (HTTP 429 / gRPC ResourceExhausted when exceeded)")
	helpFlag := flag.Bool("help", false, "print usage and exit")
	flag.Parse()

	if *helpFlag {
		flag.Usage()
		os.Exit(0)
	}

	// ---------------------------------------------------------------------------
	// Logger.
	// ---------------------------------------------------------------------------
	log := observability.NewLogger(*logLevel)
	slog.SetDefault(log)

	// ---------------------------------------------------------------------------
	// OpenTelemetry — initialise before other subsystems so spans are active.
	// Non-fatal: a missing OTLP collector must not prevent the server from starting.
	// ADR-0782: OTel tracing wired across all Go binaries.
	// ---------------------------------------------------------------------------
	otelShutdown := observability.InitOTel(context.Background(), "vmafx-server", log)
	defer func() {
		if err := otelShutdown(context.Background()); err != nil {
			log.Warn("otel shutdown error", "error", err)
		}
	}()

	log.Info("vmafx-server starting",
		"version", version(),
		"http_port", *port,
		"grpc_port", *grpcPort,
		"vmaf_binary", *vmafBinary,
		"model_dir", *modelDir,
		"max_concurrent_scores", *maxConcurrentScores,
	)

	// ---------------------------------------------------------------------------
	// libvmaf scorer.
	// ---------------------------------------------------------------------------
	scorer, err := libvmaf.New(*vmafBinary, *modelDir)
	if err != nil {
		log.Error("failed to initialise scorer", "error", err)
		os.Exit(1)
	}
	defer scorer.Close()

	// ---------------------------------------------------------------------------
	// Prometheus registry + metrics.
	// ---------------------------------------------------------------------------
	registry := prometheus.NewRegistry()
	// Register Go runtime collectors (collectors.* replaces deprecated
	// prometheus.NewGoCollector / NewProcessCollector — staticcheck SA1019).
	registry.MustRegister(collectors.NewGoCollector())
	registry.MustRegister(collectors.NewProcessCollector(collectors.ProcessCollectorOpts{}))
	metrics := observability.NewMetrics(registry)

	// ---------------------------------------------------------------------------
	// Shutdown context — cancelled on SIGTERM / SIGINT.
	// ---------------------------------------------------------------------------
	ctx, stop := observability.NewShutdownContext()
	defer stop()

	// ---------------------------------------------------------------------------
	// Concurrency limiter — shared across HTTP and gRPC so the cap is
	// enforced system-wide (not per-protocol).
	// ---------------------------------------------------------------------------
	limiter, err := NewScoreLimiter(*maxConcurrentScores)
	if err != nil {
		log.Error("failed to create concurrency limiter", "error", err)
		os.Exit(1)
	}
	log.Info("concurrency cap configured", "max_concurrent_scores", limiter.Max())

	// ---------------------------------------------------------------------------
	// Start HTTP + gRPC servers concurrently.
	// ---------------------------------------------------------------------------
	grp, grpCtx := errgroup.WithContext(ctx)

	// Shared gRPC service implementation — also consumed by the REST adapter
	// (ADR-0797) so scoring + health business logic is not duplicated.
	grpcSrv := newGRPCServerWithLimiter(scorer, metrics, log, limiter)

	// HTTP server — includes legacy endpoints, OpenAPI REST adapter, and Swagger UI.
	grp.Go(func() error {
		hs := newHTTPServerWithLimiter(scorer, metrics, registry, log, grpcSrv, limiter)
		return runHTTP(grpCtx, fmt.Sprintf(":%s", *port), hs, log)
	})

	// gRPC server.
	grp.Go(func() error {
		return runGRPCWithServer(grpCtx, fmt.Sprintf(":%s", *grpcPort), grpcSrv)
	})

	if err := grp.Wait(); err != nil {
		log.Error("server error", "error", err)
		os.Exit(1)
	}

	log.Info("vmafx-server stopped")
}

// version returns a build-time version string, falling back to "dev".
// Populated by ldflags: -ldflags "-X main.buildVersion=v1.2.3".
var buildVersion = "dev"

func version() string { return buildVersion }

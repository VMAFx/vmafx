// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/main.go — vmafx-controller entry point.
//
// The controller is the cluster brain for the VMAFX distributed platform
// (ADR-0709, ADR-0711).  It exposes:
//   - gRPC VmafxScoring service on VMAFX_GRPC_PORT (default :50051)  — direct scoring
//   - gRPC VmafxController service on VMAFX_GRPC_PORT                — job queue + node API
//   - HTTP /healthz /readyz /metrics /v1/score on VMAFX_PORT (default :8080)
//
// Starts both the HTTP and gRPC servers concurrently.  Signal handling and
// graceful shutdown are wired through pkg/observability.
//
// 12-factor (§III) environment variables:
//
//	VMAFX_PORT             HTTP listen port (default "8080").
//	VMAFX_GRPC_PORT        gRPC listen port (default "50051").
//	VMAFX_LOG_LEVEL        slog level string (default "INFO").
//	VMAFX_VMAF_BINARY      Path to the vmaf CLI binary.
//	VMAFX_MODEL_DIR        Directory containing VMAF .json model files.
//	VMAFX_DB_PATH          Path to the SQLite database (default "vmafx-controller.db").
//
// CLI flags mirror and override the environment variables.
//
// ADR-0703: vmafx-server Go gRPC + HTTP service (origin).
// ADR-0711: vmafx-controller Phase 4b.1 scope expansion.

//go:build cgo

package main

import (
	"flag"
	"fmt"
	"log/slog"
	"os"

	"github.com/prometheus/client_golang/prometheus"
	"golang.org/x/sync/errgroup"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/nodes"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/queue"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/scheduler"
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
	port := flag.String("port", envOr("VMAFX_PORT", "8080"), "HTTP listen port")
	grpcPort := flag.String("grpc-port", envOr("VMAFX_GRPC_PORT", "50051"), "gRPC listen port")
	logLevel := flag.String("log-level", envOr("VMAFX_LOG_LEVEL", "INFO"), "log level (DEBUG|INFO|WARN|ERROR)")
	vmafBinary := flag.String("vmaf-binary", envOr("VMAFX_VMAF_BINARY", ""), "path to vmaf CLI binary (default: PATH lookup)")
	modelDir := flag.String("model-dir", envOr("VMAFX_MODEL_DIR", ""), "directory containing VMAF .json model files")
	dbPath := flag.String("db", envOr("VMAFX_DB_PATH", "vmafx-controller.db"), "path to the SQLite database for job + node persistence")
	flag.Parse()

	// ---------------------------------------------------------------------------
	// Logger.
	// ---------------------------------------------------------------------------
	log := observability.NewLogger(*logLevel)
	slog.SetDefault(log)

	log.Info("vmafx-controller starting",
		"version", version(),
		"http_port", *port,
		"grpc_port", *grpcPort,
		"vmaf_binary", *vmafBinary,
		"model_dir", *modelDir,
		"db_path", *dbPath,
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
	registry.MustRegister(prometheus.NewGoCollector())
	registry.MustRegister(prometheus.NewProcessCollector(prometheus.ProcessCollectorOpts{}))
	metrics := observability.NewMetrics(registry)

	// ---------------------------------------------------------------------------
	// Controller subsystems: job queue, node registry, scheduler.
	// ---------------------------------------------------------------------------
	jobQueue, err := queue.New(*dbPath, log)
	if err != nil {
		log.Error("failed to open job queue", "error", err)
		os.Exit(1)
	}
	defer jobQueue.Close()

	nodeRegistry := nodes.NewRegistry(log)
	sched := scheduler.New(jobQueue, nodeRegistry, log)
	metrics.SetControllerSources(jobQueue, nodeRegistry)

	// ---------------------------------------------------------------------------
	// Shutdown context — cancelled on SIGTERM / SIGINT.
	// ---------------------------------------------------------------------------
	ctx, stop := observability.NewShutdownContext()
	defer stop()

	// ---------------------------------------------------------------------------
	// Start HTTP + gRPC servers concurrently.
	// ---------------------------------------------------------------------------
	grp, grpCtx := errgroup.WithContext(ctx)

	// HTTP server.
	grp.Go(func() error {
		hs := newHTTPServer(scorer, metrics, registry, log)
		return runHTTP(grpCtx, fmt.Sprintf(":%s", *port), hs, log)
	})

	// gRPC server — registers both VmafxScoring and VmafxController services.
	grp.Go(func() error {
		return runGRPC(grpCtx, fmt.Sprintf(":%s", *grpcPort), scorer, jobQueue, nodeRegistry, sched, metrics, log)
	})

	if err := grp.Wait(); err != nil {
		log.Error("controller error", "error", err)
		os.Exit(1)
	}

	log.Info("vmafx-controller stopped")
}

// version returns a build-time version string, falling back to "dev".
// Populated by ldflags: -ldflags "-X main.buildVersion=v1.2.3".
var buildVersion = "dev"

func version() string { return buildVersion }

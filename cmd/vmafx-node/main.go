// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// vmafx-node is the VMAFX worker-node binary.
//
// Each node:
//   - Discovers the ffmpeg binary (VMAFX_FFMPEG_BIN env or PATH fallback).
//   - Runs a startup probe that enumerates available encoders via
//     `ffmpeg -encoders` and caches the result.
//   - Exposes a gRPC service (cmd/vmafx-node/server.go) that the
//     vmafx-controller dispatches encoding jobs to.
//
// Phase 4b.4: ffmpeg is baked into the node Docker image at /usr/local/bin/ffmpeg
// (FFmpeg n8.2 with ffmpeg-patches/ applied — ADR-0717). The node binary
// discovers it via VMAFX_FFMPEG_BIN or PATH lookup so tests can supply a stub.
package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/VMAFx/vmafx/cmd/vmafx-node/probe"
	"github.com/VMAFx/vmafx/cmd/vmafx-node/server"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
	"github.com/VMAFx/vmafx/pkg/observability"
)

// buildVersion is stamped by the Go linker at build time:
//
//	-ldflags "-X main.buildVersion=$(git describe --tags --always --dirty)"
var buildVersion = "dev"

func main() {
	if err := run(); err != nil {
		slog.Error("vmafx-node fatal", "error", err)
		os.Exit(1)
	}
}

func run() error {
	// --help: print usage and exit 0.
	help := flag.Bool("help", false, "print usage and exit")
	flag.Usage = func() {
		fmt.Fprintf(os.Stdout, "Usage of vmafx-node:\n")
		fmt.Fprintf(os.Stdout, "  vmafx-node is configured via environment variables:\n")
		fmt.Fprintf(os.Stdout, "    VMAFX_NODE_ADDR    gRPC listen address (default :50052)\n")
		fmt.Fprintf(os.Stdout, "    VMAFX_FFMPEG_BIN   path to ffmpeg binary (default: PATH lookup)\n")
		fmt.Fprintf(os.Stdout, "    VMAFX_LOG_LEVEL    log level DEBUG|INFO|WARN|ERROR (default INFO)\n")
		fmt.Fprintf(os.Stdout, "  Flags:\n")
		flag.PrintDefaults()
	}
	flag.Parse()
	if *help {
		flag.Usage()
		os.Exit(0)
	}

	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{
		Level: logLevel(),
	}))
	slog.SetDefault(logger)

	slog.Info("vmafx-node starting", "version", buildVersion)

	// OpenTelemetry — non-fatal; missing OTLP collector must not block startup.
	// ADR-0782: OTel tracing wired across all Go binaries.
	otelShutdown := observability.InitOTel(context.Background(), "vmafx-node", logger)
	defer func() {
		if err := otelShutdown(context.Background()); err != nil {
			slog.Warn("otel shutdown error", "error", err)
		}
	}()

	ffmpegBin := ffmpegPath()
	slog.Info("ffmpeg discovery", "path", ffmpegBin)

	// Startup probe: enumerate available encoders and cache.  Bound the probe
	// with a short timeout so a hung ffmpeg binary cannot stall node startup
	// indefinitely.
	probeCtx, probeCancel := context.WithTimeout(context.Background(), 30*time.Second)
	encoders, err := probe.EncoderInventory(probeCtx, ffmpegBin)
	probeCancel()
	if err != nil {
		// Non-fatal: node can still serve; encoders that are unavailable
		// will fail at job-dispatch time with a clear error.
		slog.Warn("encoder probe failed — node will serve with empty inventory",
			"error", err, "ffmpeg", ffmpegBin)
		encoders = probe.EmptyInventory()
	} else {
		slog.Info("encoder inventory", "count", len(encoders.Available),
			"available", encoders.Available)
	}

	// Listen address from VMAFX_NODE_ADDR or default :50052.
	addr := envOrDefault("VMAFX_NODE_ADDR", ":50052")

	// Construct the libvmaf scorer that backs the VmafxScoring Score /
	// ScoreStream RPCs (ADR-0713, ADR-0933). Resolution: VMAF_BIN env / the
	// installed binary / the in-tree build (see libvmaf.FindBinary). A missing
	// binary is non-fatal — the node still serves Health for liveness probes
	// and returns FailedPrecondition from the scoring RPCs.
	modelDir := os.Getenv("VMAFX_MODEL_DIR")
	scorer, err := libvmaf.New(libvmaf.FindBinary(), modelDir)
	if err != nil {
		slog.Warn("vmaf scorer unavailable — node serves Health only",
			"error", err)
		scorer = nil
	}

	srv, err := server.New(server.Config{
		Addr:      addr,
		FFmpegBin: ffmpegBin,
		Encoders:  encoders,
		Scorer:    scorer,
		Logger:    logger,
	})
	if err != nil {
		return fmt.Errorf("create server: %w", err)
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT)
	defer stop()

	slog.Info("vmafx-node listening", "addr", addr)

	if err := srv.Serve(ctx); err != nil {
		return fmt.Errorf("serve: %w", err)
	}

	slog.Info("vmafx-node shutdown complete")
	return nil
}

// ffmpegPath resolves the ffmpeg binary path. Checks VMAFX_FFMPEG_BIN first,
// then falls back to "ffmpeg" (PATH lookup). The node Dockerfile bakes ffmpeg
// at /usr/local/bin/ffmpeg and sets VMAFX_FFMPEG_BIN accordingly (ADR-0717).
func ffmpegPath() string {
	if v := os.Getenv("VMAFX_FFMPEG_BIN"); v != "" {
		return v
	}
	return "ffmpeg"
}

// logLevel reads VMAFX_LOG_LEVEL and returns the matching slog.Level.
// Defaults to INFO.
func logLevel() slog.Level {
	switch os.Getenv("VMAFX_LOG_LEVEL") {
	case "DEBUG", "debug":
		return slog.LevelDebug
	case "WARN", "warn", "WARNING", "warning":
		return slog.LevelWarn
	case "ERROR", "error":
		return slog.LevelError
	default:
		return slog.LevelInfo
	}
}

// envOrDefault returns env variable v or fallback when v is unset.
func envOrDefault(v, fallback string) string {
	if s := os.Getenv(v); s != "" {
		return s
	}
	return fallback
}

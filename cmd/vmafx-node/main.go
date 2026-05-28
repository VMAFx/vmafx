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
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"syscall"

	"github.com/VMAFx/vmafx/cmd/vmafx-node/probe"
	"github.com/VMAFx/vmafx/cmd/vmafx-node/server"
)

// buildVersion is stamped by the Go linker at build time:
//
//	-ldflags "-X main.buildVersion=$(git describe --tags --always --dirty)"
var buildVersion = "dev"

func main() {
	if err := run(); err != nil {
		slog.Error("vmafx-node fatal", "err", err)
		os.Exit(1)
	}
}

func run() error {
	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{
		Level: logLevel(),
	}))
	slog.SetDefault(logger)

	slog.Info("vmafx-node starting", "version", buildVersion)

	ffmpegBin := ffmpegPath()
	slog.Info("ffmpeg discovery", "path", ffmpegBin)

	// Startup probe: enumerate available encoders and cache.
	encoders, err := probe.EncoderInventory(ffmpegBin)
	if err != nil {
		// Non-fatal: node can still serve; encoders that are unavailable
		// will fail at job-dispatch time with a clear error.
		slog.Warn("encoder probe failed — node will serve with empty inventory",
			"err", err, "ffmpeg", ffmpegBin)
		encoders = probe.EmptyInventory()
	} else {
		slog.Info("encoder inventory", "count", len(encoders.Available),
			"available", encoders.Available)
	}

	// Listen address from VMAFX_NODE_ADDR or default :50052.
	addr := envOrDefault("VMAFX_NODE_ADDR", ":50052")

	srv, err := server.New(server.Config{
		Addr:      addr,
		FFmpegBin: ffmpegBin,
		Encoders:  encoders,
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

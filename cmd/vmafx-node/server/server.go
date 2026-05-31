// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package server provides the vmafx-node gRPC service scaffold.
//
// Phase 4b.4 scope: the gRPC service is a stub that accepts the controller's
// job-dispatch calls and logs them. The full encoding pipeline (rclone mounts,
// GPU backend selection, sidecar training feed) is Phase 4b.5+.
package server

import (
	"context"
	"fmt"
	"log/slog"
	"net"

	"github.com/VMAFx/vmafx/cmd/vmafx-node/probe"
)

// Config holds the vmafx-node server configuration.
type Config struct {
	// Addr is the gRPC listen address (e.g., ":50052").
	Addr string

	// FFmpegBin is the path to the ffmpeg binary.
	// Populated from VMAFX_FFMPEG_BIN or "ffmpeg" by main.go.
	FFmpegBin string

	// Encoders is the cached encoder inventory from the startup probe.
	Encoders *probe.Inventory

	// Logger is the structured logger for this server instance.
	Logger *slog.Logger
}

// Server is the vmafx-node gRPC service.
type Server struct {
	cfg Config
}

// New creates a new Server. Returns an error when the config is invalid.
func New(cfg Config) (*Server, error) {
	if cfg.Addr == "" {
		return nil, fmt.Errorf("server.Config.Addr must not be empty")
	}
	if cfg.FFmpegBin == "" {
		return nil, fmt.Errorf("server.Config.FFmpegBin must not be empty")
	}
	if cfg.Encoders == nil {
		return nil, fmt.Errorf("server.Config.Encoders must not be nil")
	}
	if cfg.Logger == nil {
		cfg.Logger = slog.Default()
	}
	return &Server{cfg: cfg}, nil
}

// Serve starts the gRPC listener and blocks until ctx is cancelled.
// The full gRPC service registration is wired in Phase 4b.5 when the
// controller proto is finalized; for now the server accepts TCP connections
// and logs readiness so the Phase 4b.4 smoke test can verify the binary
// starts cleanly.
func (s *Server) Serve(ctx context.Context) error {
	ln, err := net.Listen("tcp", s.cfg.Addr)
	if err != nil {
		return fmt.Errorf("listen %s: %w", s.cfg.Addr, err)
	}
	defer func() {
		if closeErr := ln.Close(); closeErr != nil {
			s.cfg.Logger.Warn("listener close error", "error", closeErr)
		}
	}()

	s.cfg.Logger.Info("vmafx-node ready",
		"addr", s.cfg.Addr,
		"ffmpeg", s.cfg.FFmpegBin,
		"encoders_available", len(s.cfg.Encoders.Available),
		"encoders_missing", len(s.cfg.Encoders.Missing),
	)

	// Block until context is cancelled (signal or timeout).
	<-ctx.Done()
	s.cfg.Logger.Info("vmafx-node shutdown initiated", "reason", ctx.Err())
	return nil
}

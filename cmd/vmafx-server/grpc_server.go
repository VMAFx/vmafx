// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/grpc_server.go — gRPC server implementation for VMAFX.
//
// Implements the VmafxScoring service defined in proto/vmafx.proto.
// The server delegates to pkg/libvmaf for actual scoring and pkg/observability
// for Prometheus instrumentation.
//
// ADR-0703: vmafx-server Go gRPC + HTTP service.
// ADR-0933: ScoreStream bidirectional RPC (Phase 1 — schema + stub).

//go:build cgo

package main

import (
	"context"
	"fmt"
	"log/slog"
	"net"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
	"github.com/VMAFx/vmafx/pkg/observability"
)

// grpcServer implements vmafxv1.VmafxScoringServer.
type grpcServer struct {
	vmafxv1.UnimplementedVmafxScoringServer
	scorer  *libvmaf.Scorer
	metrics *observability.Metrics
	log     *slog.Logger
}

// newGRPCServer wires up the gRPC service.
func newGRPCServer(
	scorer *libvmaf.Scorer,
	metrics *observability.Metrics,
	log *slog.Logger,
) *grpcServer {
	return &grpcServer{scorer: scorer, metrics: metrics, log: log}
}

// Score implements VmafxScoring.Score.
func (s *grpcServer) Score(ctx context.Context, req *vmafxv1.ScoreRequest) (*vmafxv1.ScoreResponse, error) {
	s.metrics.ScoreRequests.Inc()
	start := time.Now()

	s.log.Info("grpc Score request",
		"reference", req.GetReference(),
		"distorted", req.GetDistorted(),
		"model", req.GetModel(),
	)

	if req.GetReference() == "" || req.GetDistorted() == "" {
		s.metrics.ScoreErrors.Inc()
		return nil, status.Errorf(codes.InvalidArgument, "reference and distorted paths are required")
	}

	score, features, err := s.scorer.Score(req.GetReference(), req.GetDistorted(), req.GetModel())
	elapsed := time.Since(start).Seconds()
	s.metrics.ScoreDuration.Observe(elapsed)

	if err != nil {
		s.metrics.ScoreErrors.Inc()
		s.log.Error("grpc Score failed", "error", err, "duration_s", elapsed)
		return nil, status.Errorf(codes.Internal, "scoring failed: %v", err)
	}

	// Convert map[string]float64 → map[string]float64 (proto uses float64 doubles).
	protoFeatures := make(map[string]float64, len(features))
	for k, v := range features {
		protoFeatures[k] = v
	}

	s.log.Info("grpc Score completed", "score", fmt.Sprintf("%.4f", score), "duration_s", elapsed)
	return &vmafxv1.ScoreResponse{
		Score:    score,
		Features: protoFeatures,
	}, nil
}

// Health implements VmafxScoring.Health.
func (s *grpcServer) Health(_ context.Context, _ *vmafxv1.HealthRequest) (*vmafxv1.HealthResponse, error) {
	s.metrics.HealthRequests.Inc()
	return &vmafxv1.HealthResponse{Ok: true, Message: "ok"}, nil
}

// ScoreStream implements VmafxScoring.ScoreStream.
//
// Phase 1 stub (ADR-0933): the schema and dispatch are wired end-to-end so
// clients can compile against the new surface and exercise gRPC-level
// behaviour (auth, TLS, deadlines). The actual per-frame scoring is wired in
// Phase 2, which adds an in-memory picture-import path to pkg/libvmaf.
//
// Until then this handler returns codes.Unimplemented, but it explicitly
// validates the opening message so clients learn early whether they framed
// the stream correctly. We deliberately do NOT rely on the
// UnimplementedVmafxScoringServer default — overriding here lets the Phase 2
// implementation drop in without changing this file's signature surface.
func (s *grpcServer) ScoreStream(stream vmafxv1.VmafxScoring_ScoreStreamServer) error {
	s.log.Info("grpc ScoreStream request received (Phase 1 stub — ADR-0933)")

	// Read the opening message to validate framing: it MUST be a StreamConfig.
	first, err := stream.Recv()
	if err != nil {
		s.log.Error("grpc ScoreStream: failed to read opening message", "error", err)
		return status.Errorf(codes.InvalidArgument, "ScoreStream requires an opening StreamConfig message: %v", err)
	}
	cfg := first.GetConfig()
	if cfg == nil {
		return status.Errorf(codes.InvalidArgument, "ScoreStream: first message must set the `config` oneof (StreamConfig), got payload=%T", first.GetPayload())
	}
	if cfg.GetWidth() == 0 || cfg.GetHeight() == 0 {
		return status.Errorf(codes.InvalidArgument, "ScoreStream: StreamConfig requires non-zero width and height (got %dx%d)", cfg.GetWidth(), cfg.GetHeight())
	}
	if cfg.GetPixelFormat() == vmafxv1.PixelFormat_PIXEL_FORMAT_UNSPECIFIED {
		return status.Errorf(codes.InvalidArgument, "ScoreStream: StreamConfig.pixel_format must be set")
	}

	s.log.Info("grpc ScoreStream: config accepted, but per-frame scoring is not implemented yet",
		"width", cfg.GetWidth(),
		"height", cfg.GetHeight(),
		"pixel_format", cfg.GetPixelFormat().String(),
		"model", cfg.GetModel(),
		"frame_count_hint", cfg.GetFrameCountHint(),
	)

	// Phase 1: scoring not implemented. Return Unimplemented so the client
	// learns immediately rather than after pushing N gigabytes of frames.
	return status.Errorf(codes.Unimplemented, "ScoreStream per-frame scoring is wired in Phase 2 (ADR-0933); Phase 1 ships the schema + framing validation only")
}

// runGRPC starts the gRPC listener on addr (e.g. ":50051") and blocks
// until ctx is cancelled.
func runGRPC(
	ctx context.Context,
	addr string,
	scorer *libvmaf.Scorer,
	metrics *observability.Metrics,
	log *slog.Logger,
) error {
	lis, err := net.Listen("tcp", addr)
	if err != nil {
		return fmt.Errorf("grpc listen %s: %w", addr, err)
	}

	srv := grpc.NewServer()
	vmafxv1.RegisterVmafxScoringServer(srv, newGRPCServer(scorer, metrics, log))

	log.Info("gRPC server started", "addr", addr)

	// Shut down gracefully when ctx is cancelled.
	go func() {
		<-ctx.Done()
		log.Info("gRPC graceful shutdown initiated")
		srv.GracefulStop()
	}()

	if err := srv.Serve(lis); err != nil {
		return fmt.Errorf("grpc serve: %w", err)
	}
	return nil
}

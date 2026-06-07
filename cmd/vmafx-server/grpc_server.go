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
	"runtime/debug"
	"time"

	"go.opentelemetry.io/contrib/instrumentation/google.golang.org/grpc/otelgrpc"
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

	// Pass the gRPC handler context so a client disconnect or RPC
	// deadline tears down the vmaf subprocess via exec.CommandContext.
	// Fixes T-LIBVMAF-SCORE-NEEDS-CTX-2026-05-31.
	score, features, err := s.scorer.Score(ctx, req.GetReference(), req.GetDistorted(), req.GetModel())
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
// until ctx is cancelled.  It creates a new grpcServer internally.
//
// Deprecated: prefer runGRPCWithServer when a shared grpcServer instance is
// needed (e.g. to wire the REST adapter per ADR-0797).
func runGRPC(
	ctx context.Context,
	addr string,
	scorer *libvmaf.Scorer,
	metrics *observability.Metrics,
	log *slog.Logger,
) error {
	return runGRPCWithServer(ctx, addr, newGRPCServer(scorer, metrics, log))
}

// runGRPCWithServer starts the gRPC listener on addr using a pre-constructed
// grpcServer.  This allows the same grpcServer instance to be shared with the
// HTTP REST adapter (ADR-0797) so business logic is not duplicated.
func runGRPCWithServer(
	ctx context.Context,
	addr string,
	impl *grpcServer,
) error {
	lis, err := net.Listen("tcp", addr)
	if err != nil {
		return fmt.Errorf("grpc listen %s: %w", addr, err)
	}

	// ADR-0927: OTel stats handler instruments every gRPC RPC with a server
	// span and extracts the W3C traceparent / tracestate headers from the
	// incoming request metadata so server spans appear as children of the
	// calling trace.  No-op when InitOTel installed no-op providers.
	// ADR-0978: panic-recovery interceptors so a misbehaving handler surfaces
	// as codes.Internal rather than crashing the process.
	srv := grpc.NewServer(
		grpc.StatsHandler(otelgrpc.NewServerHandler()),
		grpc.UnaryInterceptor(recoveryUnaryInterceptor(impl.log)),
		grpc.StreamInterceptor(recoveryStreamInterceptor(impl.log)),
	)
	vmafxv1.RegisterVmafxScoringServer(srv, impl)

	impl.log.Info("gRPC server started", "addr", addr)

	// Shut down gracefully when ctx is cancelled.
	// GracefulStop waits for all in-flight RPCs; add a hard-stop fallback so a
	// stuck streaming RPC cannot block shutdown forever (r3-signal finding).
	go func() {
		<-ctx.Done()
		impl.log.Info("gRPC graceful shutdown initiated")
		done := make(chan struct{})
		go func() {
			srv.GracefulStop()
			close(done)
		}()
		select {
		case <-done:
		case <-time.After(observability.GracefulShutdownTimeout):
			impl.log.Warn("gRPC graceful stop timed out; forcing hard stop")
			srv.Stop()
		}
	}()

	if err := srv.Serve(lis); err != nil {
		return fmt.Errorf("grpc serve: %w", err)
	}
	return nil
}

// recoveryUnaryInterceptor returns a UnaryServerInterceptor that converts a
// panic inside the handler into a codes.Internal gRPC status, logging the
// stack trace at ERROR level. Without this a panic in any handler tears down
// the gRPC server's worker goroutine; the gRPC library then re-raises it in
// the connection's read loop and crashes the process. ADR-0978.
func recoveryUnaryInterceptor(log *slog.Logger) grpc.UnaryServerInterceptor {
	return func(
		ctx context.Context,
		req interface{},
		info *grpc.UnaryServerInfo,
		handler grpc.UnaryHandler,
	) (resp interface{}, err error) {
		defer func() {
			if p := recover(); p != nil {
				log.Error("grpc unary handler panic recovered",
					"method", info.FullMethod,
					"panic", fmt.Sprintf("%v", p),
					"stack", string(debug.Stack()),
				)
				err = status.Errorf(codes.Internal, "internal server error")
			}
		}()
		return handler(ctx, req)
	}
}

// recoveryStreamInterceptor is the streaming counterpart of
// recoveryUnaryInterceptor. ADR-0978.
func recoveryStreamInterceptor(log *slog.Logger) grpc.StreamServerInterceptor {
	return func(
		srv interface{},
		ss grpc.ServerStream,
		info *grpc.StreamServerInfo,
		handler grpc.StreamHandler,
	) (err error) {
		defer func() {
			if p := recover(); p != nil {
				log.Error("grpc stream handler panic recovered",
					"method", info.FullMethod,
					"panic", fmt.Sprintf("%v", p),
					"stack", string(debug.Stack()),
				)
				err = status.Errorf(codes.Internal, "internal server error")
			}
		}()
		return handler(srv, ss)
	}
}

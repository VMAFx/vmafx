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
	"errors"
	"fmt"
	"io"
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
	// limiter caps concurrent in-flight Scorer.Score calls to prevent
	// unbounded subprocess forking under load (unauthenticated DoS fix).
	// nil means uncapped (tests that construct grpcServer directly without
	// a limiter retain the old behaviour; production always sets one).
	limiter *ScoreLimiter
}

// newGRPCServer wires up the gRPC service.
func newGRPCServer(
	scorer *libvmaf.Scorer,
	metrics *observability.Metrics,
	log *slog.Logger,
) *grpcServer {
	return &grpcServer{scorer: scorer, metrics: metrics, log: log}
}

// newGRPCServerWithLimiter wires up the gRPC service with a concurrency cap.
// Prefer this constructor in production; newGRPCServer is kept for test
// compatibility.
func newGRPCServerWithLimiter(
	scorer *libvmaf.Scorer,
	metrics *observability.Metrics,
	log *slog.Logger,
	limiter *ScoreLimiter,
) *grpcServer {
	return &grpcServer{scorer: scorer, metrics: metrics, log: log, limiter: limiter}
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

	// Enforce the concurrency cap before forking a vmaf subprocess.
	// Acquire blocks until a slot is free or ctx is cancelled; a cancelled ctx
	// means the client already gave up, so we reject immediately rather than
	// wasting a slot on a dead request.
	if s.limiter != nil {
		if err := s.limiter.Acquire(ctx); err != nil {
			s.metrics.ScoreErrors.Inc()
			s.log.Warn("grpc Score rejected: concurrency cap reached",
				"max", s.limiter.Max(), "error", err)
			return nil, status.Errorf(codes.ResourceExhausted,
				"too many concurrent scoring requests (max %d); try again later", s.limiter.Max())
		}
		defer s.limiter.Release()
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

// ScoreStream implements VmafxScoring.ScoreStream (ADR-0933 Phase 2).
//
// The bidirectional contract (proto/vmafx.proto):
//
//   - The client sends exactly one StreamConfig as the opening message, then a
//     sequence of FramePair messages with strictly increasing frame_index from
//     0, then half-closes.
//   - The server validates each frame, feeds the raw planar bytes into the
//     in-process libvmaf StreamScorer, and after the client half-closes
//     flushes the engine and streams back one FrameScore per frame followed by
//     exactly one terminal AggregateScore.
//
// Per-frame scores are emitted after EOF rather than incrementally because
// several VMAF features (motion) are temporal and only finalise once the whole
// sequence has been read and flushed — see pkg/libvmaf/stream.go. The
// ScoreStreamResponse oneof (N frame_score then one aggregate) is honoured
// either way; clients that stream gigabytes still benefit from gRPC flow
// control on the request side while frames are pushed.
//
// Cancellation: stream.Context() (cancelled on client disconnect or deadline)
// is propagated into the score harvest and checked between received frames, so
// a dropped client tears the scorer down promptly. The StreamScorer is always
// Closed via defer so the libvmaf context is released on every exit path.
func (s *grpcServer) ScoreStream(stream vmafxv1.VmafxScoring_ScoreStreamServer) error {
	ctx := stream.Context()
	s.metrics.ScoreRequests.Inc()
	start := time.Now()
	s.log.Info("grpc ScoreStream request received (ADR-0933)")

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

	pixFmt, bitDepth, err := protoPixelFormat(cfg.GetPixelFormat())
	if err != nil {
		return status.Errorf(codes.InvalidArgument, "ScoreStream: %v", err)
	}

	// Resolve the requested model to an absolute path via the same search
	// order the unary Score path uses.
	modelPath, err := s.scorer.ResolveModel(cfg.GetModel())
	if err != nil {
		s.metrics.ScoreErrors.Inc()
		return status.Errorf(codes.InvalidArgument, "ScoreStream: model %q: %v", cfg.GetModel(), err)
	}

	// A streaming call holds an in-process libvmaf context for its whole
	// lifetime; treat it like a unary Score for concurrency accounting so a
	// flood of streams cannot exhaust memory.
	if s.limiter != nil {
		if acqErr := s.limiter.Acquire(ctx); acqErr != nil {
			s.metrics.ScoreErrors.Inc()
			s.log.Warn("grpc ScoreStream rejected: concurrency cap reached",
				"max", s.limiter.Max(), "error", acqErr)
			return status.Errorf(codes.ResourceExhausted,
				"too many concurrent scoring requests (max %d); try again later", s.limiter.Max())
		}
		defer s.limiter.Release()
	}

	scorer, err := libvmaf.NewStreamScorer(libvmaf.StreamConfig{
		Width:          int(cfg.GetWidth()),
		Height:         int(cfg.GetHeight()),
		PixFmt:         pixFmt,
		BitDepth:       bitDepth,
		ModelPath:      modelPath,
		FrameCountHint: int(cfg.GetFrameCountHint()),
	})
	if err != nil {
		s.metrics.ScoreErrors.Inc()
		s.log.Error("grpc ScoreStream: scorer init failed", "error", err)
		return streamScorerStatus(err)
	}
	defer scorer.Close()

	s.log.Info("grpc ScoreStream: config accepted",
		"width", cfg.GetWidth(),
		"height", cfg.GetHeight(),
		"pixel_format", cfg.GetPixelFormat().String(),
		"model", modelPath,
		"frame_count_hint", cfg.GetFrameCountHint(),
		"frame_size_bytes", scorer.FrameSize(),
	)

	// Ingest frames until the client half-closes (io.EOF).
	for {
		if err := ctx.Err(); err != nil {
			return status.FromContextError(err).Err()
		}
		msg, recvErr := stream.Recv()
		if recvErr == io.EOF {
			break
		}
		if recvErr != nil {
			// A non-EOF receive error is usually a client cancellation or a
			// broken connection; surface the context error when present.
			if ctxErr := ctx.Err(); ctxErr != nil {
				return status.FromContextError(ctxErr).Err()
			}
			s.metrics.ScoreErrors.Inc()
			return status.Errorf(codes.Internal, "ScoreStream: receive frame: %v", recvErr)
		}
		fp := msg.GetFramePair()
		if fp == nil {
			s.metrics.ScoreErrors.Inc()
			return status.Errorf(codes.InvalidArgument,
				"ScoreStream: post-config message must set the `frame_pair` oneof, got payload=%T", msg.GetPayload())
		}
		if pushErr := scorer.PushFrame(int(fp.GetFrameIndex()), fp.GetRawReference(), fp.GetRawDistorted()); pushErr != nil {
			s.metrics.ScoreErrors.Inc()
			return streamScorerStatus(pushErr)
		}
	}

	// Flush + harvest per-frame and pooled scores.
	result, err := scorer.Finish(ctx)
	if err != nil {
		s.metrics.ScoreErrors.Inc()
		s.log.Error("grpc ScoreStream: finish failed", "error", err)
		return streamScorerStatus(err)
	}

	// Stream back one FrameScore per processed frame.
	for _, fr := range result.Frames {
		if err := ctx.Err(); err != nil {
			return status.FromContextError(err).Err()
		}
		if sendErr := stream.Send(&vmafxv1.ScoreStreamResponse{
			Payload: &vmafxv1.ScoreStreamResponse_FrameScore{
				FrameScore: &vmafxv1.FrameScore{
					FrameIndex: uint32(fr.Index),
					Score:      fr.Score,
					Features:   fr.Features,
				},
			},
		}); sendErr != nil {
			s.log.Debug("grpc ScoreStream: Send(frame) interrupted", "error", sendErr)
			return sendErr
		}
	}

	// Terminal AggregateScore.
	elapsed := time.Since(start)
	s.metrics.ScoreDuration.Observe(elapsed.Seconds())
	if sendErr := stream.Send(&vmafxv1.ScoreStreamResponse{
		Payload: &vmafxv1.ScoreStreamResponse_Aggregate{
			Aggregate: &vmafxv1.AggregateScore{
				FramesProcessed: uint32(result.FramesProcessed),
				Score:           result.Score,
				Features:        result.Features,
				ElapsedMs:       uint64(elapsed.Milliseconds()),
			},
		},
	}); sendErr != nil {
		s.log.Debug("grpc ScoreStream: Send(aggregate) interrupted", "error", sendErr)
		return sendErr
	}

	s.log.Info("grpc ScoreStream completed",
		"frames", result.FramesProcessed,
		"score", fmt.Sprintf("%.4f", result.Score),
		"duration_s", elapsed.Seconds(),
	)
	return nil
}

// protoPixelFormat maps a proto PixelFormat enum to the libvmaf chroma layout
// plus bit depth.  Returns InvalidArgument-shaped errors for the unspecified
// value (the caller has already rejected UNSPECIFIED, but this keeps the
// mapping total).
func protoPixelFormat(pf vmafxv1.PixelFormat) (libvmaf.PixelFormat, int, error) {
	switch pf {
	case vmafxv1.PixelFormat_PIXEL_FORMAT_YUV420P:
		return libvmaf.PixFmtYUV420P, 8, nil
	case vmafxv1.PixelFormat_PIXEL_FORMAT_YUV422P:
		return libvmaf.PixFmtYUV422P, 8, nil
	case vmafxv1.PixelFormat_PIXEL_FORMAT_YUV444P:
		return libvmaf.PixFmtYUV444P, 8, nil
	case vmafxv1.PixelFormat_PIXEL_FORMAT_YUV420P10LE:
		return libvmaf.PixFmtYUV420P, 10, nil
	case vmafxv1.PixelFormat_PIXEL_FORMAT_YUV422P10LE:
		return libvmaf.PixFmtYUV422P, 10, nil
	case vmafxv1.PixelFormat_PIXEL_FORMAT_YUV444P10LE:
		return libvmaf.PixFmtYUV444P, 10, nil
	default:
		return 0, 0, fmt.Errorf("unsupported pixel_format %v", pf)
	}
}

// streamScorerStatus maps a pkg/libvmaf typed error to the right gRPC status
// code.  Invalid-argument-class errors (bad geometry, frame-size mismatch,
// out-of-order index, missing model) become codes.InvalidArgument /
// codes.NotFound; anything else is codes.Internal.
func streamScorerStatus(err error) error {
	switch {
	case errors.Is(err, libvmaf.ErrInvalidArgument):
		return status.Errorf(codes.InvalidArgument, "%v", err)
	case errors.Is(err, libvmaf.ErrModelNotFound):
		return status.Errorf(codes.NotFound, "%v", err)
	case errors.Is(err, libvmaf.ErrPictureRead):
		return status.Errorf(codes.InvalidArgument, "%v", err)
	default:
		return status.Errorf(codes.Internal, "%v", err)
	}
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

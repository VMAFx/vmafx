// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/grpc_server.go — gRPC server implementation for VMAFX.
//
// Implements the VmafxScoring service defined in proto/vmafx/v1/vmafx.proto
// (generated from api/vmafx-platform.toml, ADR-2350).
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
	"runtime/debug"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	"github.com/VMAFx/vmafx/internal/app/scoringservice"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
	"github.com/VMAFx/vmafx/pkg/observability"
)

// grpcServer implements vmafxv1.VmafxScoringServer.
type grpcServer struct {
	vmafxv1.UnimplementedVmafxScoringServer
	scorer  *libvmaf.Scorer
	metrics *observability.Metrics
	// streams records the ScoreStream sessions; nil in tests that build the
	// server without it (records nothing).
	streams *scoringservice.StreamMetrics
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

	observability.RPCLogger(ctx, s.log, "Score").Info("grpc Score request",
		"reference", req.GetReference(),
		"distorted", req.GetDistorted(),
		observability.FieldModel, req.GetModel(),
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

	resp, err := s.scoreWithOptions(ctx, req)
	elapsed := time.Since(start)
	s.metrics.ScoreDuration.Observe(elapsed.Seconds())
	log := observability.RPCLogger(ctx, s.log, "Score")
	if err != nil {
		s.metrics.ScoreErrors.Inc()
		log.Error("grpc Score failed", observability.FieldModel, req.GetModel(),
			observability.Seconds(elapsed), observability.FieldError, err)
		return nil, err
	}
	s.metrics.ObserveScore("", req.GetModel(), resp.GetScore())
	log.Info("grpc Score completed", observability.FieldModel, resp.GetProvenance().GetModel(),
		observability.FieldBackend, resp.GetProvenance().GetBackendUsed(),
		"score", fmt.Sprintf("%.4f", resp.GetScore()), observability.Seconds(elapsed))
	return resp, nil
}

// scoreWithOptions runs one request through runScore (score_options.go), the
// one scoring path gRPC Score, POST /v1/score and the REST adapter share.
func (s *grpcServer) scoreWithOptions(ctx context.Context, req *vmafxv1.ScoreRequest) (*vmafxv1.ScoreResponse, error) {
	return runScore(ctx, s.scorer, req)
}

// Health implements VmafxScoring.Health.
func (s *grpcServer) Health(_ context.Context, _ *vmafxv1.HealthRequest) (*vmafxv1.HealthResponse, error) {
	s.metrics.HealthRequests.Inc()
	return &vmafxv1.HealthResponse{Ok: true, Message: "ok"}, nil
}

// ScoreStream implements VmafxScoring.ScoreStream (ADR-0933 Phase 2).
//
// The bidirectional contract (proto/vmafx/v1/vmafx.proto):
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
// a dropped client tears the scorer down promptly. The StreamScorer is closed
// on every exit path; a failed close gets one immediate retry and a persistent
// failure is returned alongside the primary handler error.
func (s *grpcServer) ScoreStream(stream vmafxv1.VmafxScoring_ScoreStreamServer) (retErr error) {
	ctx := stream.Context()
	s.metrics.ScoreRequests.Inc()
	start := time.Now()
	session := s.streams.Begin()
	defer func() { session.End(retErr) }()
	s.log.Info("grpc ScoreStream request received (ADR-0933)")

	cfg, scorerCfg, err := s.acceptStreamConfig(stream)
	if err != nil {
		return err
	}

	release, err := s.acquireStreamSlot(ctx)
	if err != nil {
		return err
	}
	defer release()

	scorer, err := libvmaf.NewStreamScorer(scorerCfg)
	if err != nil {
		s.metrics.ScoreErrors.Inc()
		s.log.Error("grpc ScoreStream: scorer init failed", "error", err)
		return streamScorerStatus(err)
	}
	defer func() {
		retErr = s.closeStreamScorer(scorer, retErr)
	}()

	s.log.Info("grpc ScoreStream: config accepted",
		"width", cfg.GetWidth(),
		"height", cfg.GetHeight(),
		"pixel_format", cfg.GetPixelFormat().String(),
		"model", scorerCfg.ModelPath,
		"frame_count_hint", cfg.GetFrameCountHint(),
		"frame_size_bytes", scorer.FrameSize(),
	)

	if ingestErr := s.ingestFrames(ctx, stream, scorer, session); ingestErr != nil {
		return ingestErr
	}

	result, provenance, err := s.harvestStream(ctx, scorer, cfg.GetModel())
	if err != nil {
		return err
	}
	s.metrics.ObserveScore("", cfg.GetModel(), result.Score)

	if sendErr := s.sendFrameScores(ctx, stream, result.Frames); sendErr != nil {
		return sendErr
	}

	// Terminal AggregateScore.
	return s.sendAggregate(stream, result, provenance, time.Since(start))
}

// harvestStream flushes the stream and harvests the per-frame and pooled
// scores and the provenance of the in-process context (#2155).
func (s *grpcServer) harvestStream(
	ctx context.Context, scorer *libvmaf.StreamScorer, model string,
) (*libvmaf.StreamResult, *vmafxv1.ScoreProvenance, error) {
	result, err := scorer.Finish(ctx)
	if err != nil {
		s.metrics.ScoreErrors.Inc()
		s.log.Error("grpc ScoreStream: finish failed", observability.FieldError, err)
		return nil, nil, streamScorerStatus(err)
	}
	provenance, err := streamProvenance(scorer, model)
	if err != nil {
		s.metrics.ScoreErrors.Inc()
		s.log.Error("grpc ScoreStream: provenance failed", observability.FieldError, err)
		return nil, nil, status.Errorf(codes.Internal, "scoring provenance: %v", err)
	}
	return result, provenance, nil
}

// closeStreamScorer closes scorer with the request-scoped retry contract and
// preserves any handler error when teardown also fails.
func (s *grpcServer) closeStreamScorer(
	scorer *libvmaf.StreamScorer,
	operationErr error,
) error {
	closeResult := scoringservice.CloseStreamScorerWithRetry(scorer)
	if closeResult.InitialErr == nil {
		return operationErr
	}
	if closeResult.RetryErr == nil {
		s.log.Warn("grpc ScoreStream: scorer close recovered on retry",
			"error", closeResult.InitialErr)
		return operationErr
	}

	closeErr := closeResult.Err()
	s.metrics.ScoreErrors.Inc()
	s.log.Error("grpc ScoreStream: scorer close failed after retry",
		"error", closeErr,
		"operation_error", operationErr,
	)
	return errors.Join(operationErr, status.Errorf(codes.Internal,
		"ScoreStream teardown failed after retry: %v", closeErr))
}

// acquireStreamSlot takes one scoring slot for the lifetime of a streaming call and
// returns the function that hands it back.
//
// A streaming call holds an in-process libvmaf context for its whole lifetime; treating
// it like a unary Score for concurrency accounting is what stops a flood of streams from
// exhausting memory. With no limiter configured the returned release is a no-op, so the
// caller defers it unconditionally and the slot is given back on every exit path.
func (s *grpcServer) acquireStreamSlot(ctx context.Context) (func(), error) {
	if s.limiter == nil {
		return func() {}, nil
	}
	if acqErr := s.limiter.Acquire(ctx); acqErr != nil {
		s.metrics.ScoreErrors.Inc()
		s.log.Warn("grpc ScoreStream rejected: concurrency cap reached",
			"max", s.limiter.Max(), "error", acqErr)
		return nil, status.Errorf(codes.ResourceExhausted,
			"too many concurrent scoring requests (max %d); try again later", s.limiter.Max())
	}
	return s.limiter.Release, nil
}

// sendAggregate sends the terminal AggregateScore that closes a ScoreStream, records the
// call's duration, and logs the completion. It is the last message of the response stream
// by contract: N FrameScore messages followed by exactly one aggregate.
func (s *grpcServer) sendAggregate(
	stream vmafxv1.VmafxScoring_ScoreStreamServer,
	result *libvmaf.StreamResult,
	provenance *vmafxv1.ScoreProvenance,
	elapsed time.Duration,
) error {
	s.metrics.ScoreDuration.Observe(elapsed.Seconds())
	if sendErr := stream.Send(&vmafxv1.ScoreStreamResponse{
		Payload: &vmafxv1.ScoreStreamResponse_Aggregate{
			Aggregate: &vmafxv1.AggregateScore{
				FramesProcessed: libvmaf.SafeUint32(result.FramesProcessed),
				Score:           result.Score,
				Features:        result.Features,
				ElapsedMs:       libvmaf.SafeUint64(elapsed.Milliseconds()),
				Provenance:      provenance,
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

// acceptStreamConfig reads the opening message, validates that it is a well-formed
// StreamConfig, and turns it into the libvmaf stream configuration this call will run
// with. The returned *vmafxv1.StreamConfig is the client's request as sent, kept for the
// acceptance log line.
//
// It deliberately runs before the concurrency limiter is taken, so a malformed request is
// rejected without ever occupying one of the scoring slots.
func (s *grpcServer) acceptStreamConfig(
	stream vmafxv1.VmafxScoring_ScoreStreamServer,
) (*vmafxv1.StreamConfig, libvmaf.StreamConfig, error) {
	var noCfg libvmaf.StreamConfig
	// Read the opening message to validate framing: it MUST be a StreamConfig.
	first, err := stream.Recv()
	if err != nil {
		s.log.Error("grpc ScoreStream: failed to read opening message", "error", err)
		return nil, noCfg, status.Errorf(codes.InvalidArgument, "ScoreStream requires an opening StreamConfig message: %v", err)
	}
	cfg := first.GetConfig()
	if cfg == nil {
		return nil, noCfg, status.Errorf(codes.InvalidArgument, "ScoreStream: first message must set the `config` oneof (StreamConfig), got payload=%T", first.GetPayload())
	}
	if cfg.GetWidth() == 0 || cfg.GetHeight() == 0 {
		return nil, noCfg, status.Errorf(codes.InvalidArgument, "ScoreStream: StreamConfig requires non-zero width and height (got %dx%d)", cfg.GetWidth(), cfg.GetHeight())
	}
	if cfg.GetPixelFormat() == vmafxv1.PixelFormat_PIXEL_FORMAT_UNSPECIFIED {
		return nil, noCfg, status.Errorf(codes.InvalidArgument, "ScoreStream: StreamConfig.pixel_format must be set")
	}

	pixFmt, bitDepth, err := protoPixelFormat(cfg.GetPixelFormat())
	if err != nil {
		return nil, noCfg, status.Errorf(codes.InvalidArgument, "ScoreStream: %v", err)
	}

	// Resolve the requested model to an absolute path via the same search
	// order the unary Score path uses.
	modelPath, err := s.scorer.ResolveModel(cfg.GetModel())
	if err != nil {
		s.metrics.ScoreErrors.Inc()
		return nil, noCfg, status.Errorf(codes.InvalidArgument, "ScoreStream: model %q: %v", cfg.GetModel(), err)
	}

	return cfg, libvmaf.StreamConfig{
		Width:          int(cfg.GetWidth()),
		Height:         int(cfg.GetHeight()),
		PixFmt:         pixFmt,
		BitDepth:       bitDepth,
		ModelPath:      modelPath,
		FrameCountHint: int(cfg.GetFrameCountHint()),
	}, nil
}

// ingestFrames pushes every FramePair the client sends into scorer.
//
// Ingest ends when the client half-closes (io.EOF), which is what halfClosed records, or
// when the call is cancelled. The context is tested before each blocking Recv so a
// dropped client tears the scorer down promptly, and an ingest that ends without the
// half-close reports that context error rather than flushing a stream whose client is
// already gone. Every error path bumps ScoreErrors, matching the unary Score accounting.
func (s *grpcServer) ingestFrames(
	ctx context.Context,
	stream vmafxv1.VmafxScoring_ScoreStreamServer,
	scorer *libvmaf.StreamScorer,
	session *scoringservice.StreamSession,
) error {
	halfClosed := false
	for !halfClosed && ctx.Err() == nil {
		msg, recvErr := stream.Recv()
		if recvErr == io.EOF {
			halfClosed = true
			continue
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
		session.Frame()
	}
	if !halfClosed {
		return status.FromContextError(ctx.Err()).Err()
	}
	return nil
}

// sendFrameScores streams back one FrameScore per processed frame, abandoning the send as
// soon as the call is cancelled so a disconnected client cannot hold the loop open.
func (s *grpcServer) sendFrameScores(
	ctx context.Context,
	stream vmafxv1.VmafxScoring_ScoreStreamServer,
	frames []libvmaf.FrameResult,
) error {
	for _, fr := range frames {
		if err := ctx.Err(); err != nil {
			return status.FromContextError(err).Err()
		}
		if sendErr := stream.Send(&vmafxv1.ScoreStreamResponse{
			Payload: &vmafxv1.ScoreStreamResponse_FrameScore{
				FrameScore: &vmafxv1.FrameScore{
					FrameIndex: libvmaf.SafeUint32(fr.Index),
					Score:      fr.Score,
					Features:   fr.Features,
				},
			},
		}); sendErr != nil {
			s.log.Debug("grpc ScoreStream: Send(frame) interrupted", "error", sendErr)
			return sendErr
		}
	}
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

// recoveryUnaryInterceptor returns a UnaryServerInterceptor that converts a
// panic inside the handler into a codes.Internal gRPC status, logging the
// stack trace at ERROR level. Without this a panic in any handler tears down
// the gRPC server's worker goroutine; the gRPC library then re-raises it in
// the connection's read loop and crashes the process. ADR-0978.
func recoveryUnaryInterceptor(log *slog.Logger) grpc.UnaryServerInterceptor {
	return func(
		ctx context.Context,
		req any,
		info *grpc.UnaryServerInfo,
		handler grpc.UnaryHandler,
	) (resp any, err error) {
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
		srv any,
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

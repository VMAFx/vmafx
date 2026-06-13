// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package server provides the vmafx-node gRPC service.
//
// The node exposes the VmafxScoring service (proto/vmafx.proto) so a
// controller — or any gRPC client — can dispatch scoring work directly to the
// node (push model): Score for file-path unary scoring, ScoreStream for
// in-memory per-frame streaming (ADR-0933), and Health for liveness probes.
// The node binary connects libvmaf via cgo (ADR-0713), so the served scoring
// surface reuses the same pkg/libvmaf engine the standalone vmafx-server uses.
//
// Service selection (ADR-0713 / ADR-0709): the worker-side gRPC surface is the
// scoring contract, not a bespoke node API — the only scoring service the proto
// defines is VmafxScoring, and the controller's Node API (RegisterNode /
// PullWork / ReportResult) is a *client* role the node plays against the
// controller, not a service the node hosts. Registering VmafxScoring here gives
// the node a dispatchable scoring endpoint without inventing a second contract.
//
// When Config.Scorer is nil the node still serves Health (so the Phase 4b.4
// smoke test and k8s liveness probe pass) but returns codes.FailedPrecondition
// from Score / ScoreStream, since scoring requires a libvmaf-backed scorer.
package server

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"time"

	"go.opentelemetry.io/contrib/instrumentation/google.golang.org/grpc/otelgrpc"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	"github.com/VMAFx/vmafx/cmd/vmafx-node/probe"
	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
)

// gracefulShutdownTimeout bounds how long Serve waits for in-flight RPCs to
// drain on shutdown before forcing a hard stop. A stuck streaming RPC must not
// block node termination past the 30 s SIGTERM budget (ADR-0713).
const gracefulShutdownTimeout = 25 * time.Second

// Config holds the vmafx-node server configuration.
type Config struct {
	// Addr is the gRPC listen address (e.g., ":50052").
	Addr string

	// FFmpegBin is the path to the ffmpeg binary.
	// Populated from VMAFX_FFMPEG_BIN or "ffmpeg" by main.go.
	FFmpegBin string

	// Encoders is the cached encoder inventory from the startup probe.
	Encoders *probe.Inventory

	// Scorer backs the VmafxScoring Score / ScoreStream RPCs. May be nil, in
	// which case the node serves Health only and returns
	// codes.FailedPrecondition from the scoring RPCs.
	Scorer *libvmaf.Scorer

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

// Serve starts the gRPC server and blocks until ctx is cancelled.
//
// It registers the VmafxScoring service (Score, ScoreStream, Health) and shuts
// down gracefully when ctx is cancelled (SIGTERM / SIGINT via the signal
// context wired in main.go). A hard-stop fallback fires if graceful drain
// exceeds gracefulShutdownTimeout so a wedged streaming RPC cannot block node
// termination indefinitely.
func (s *Server) Serve(ctx context.Context) error {
	ln, err := net.Listen("tcp", s.cfg.Addr)
	if err != nil {
		return fmt.Errorf("listen %s: %w", s.cfg.Addr, err)
	}

	// OTel stats handler: instruments every RPC and is a no-op when InitOTel
	// installed no-op providers (as in tests).
	grpcSrv := grpc.NewServer(
		grpc.StatsHandler(otelgrpc.NewServerHandler()),
	)
	vmafxv1.RegisterVmafxScoringServer(grpcSrv, newScoringHandler(s.cfg.Scorer, s.cfg.Logger))

	s.cfg.Logger.Info("vmafx-node ready",
		"addr", ln.Addr().String(),
		"ffmpeg", s.cfg.FFmpegBin,
		"encoders_available", len(s.cfg.Encoders.Available),
		"encoders_missing", len(s.cfg.Encoders.Missing),
		"scoring_enabled", s.cfg.Scorer != nil,
		"services", []string{"VmafxScoring"},
	)

	// Drive graceful shutdown from ctx cancellation.
	go func() {
		<-ctx.Done()
		s.cfg.Logger.Info("vmafx-node shutdown initiated", "reason", ctx.Err())
		done := make(chan struct{})
		go func() {
			grpcSrv.GracefulStop()
			close(done)
		}()
		select {
		case <-done:
		case <-time.After(gracefulShutdownTimeout):
			s.cfg.Logger.Warn("vmafx-node graceful stop timed out; forcing hard stop")
			grpcSrv.Stop()
		}
	}()

	// Serve blocks until GracefulStop / Stop is called. grpc.ErrServerStopped
	// is the normal "shut down cleanly" signal and is not an error here.
	if serveErr := grpcSrv.Serve(ln); serveErr != nil && !errors.Is(serveErr, grpc.ErrServerStopped) {
		return fmt.Errorf("grpc serve: %w", serveErr)
	}
	s.cfg.Logger.Info("vmafx-node shutdown complete")
	return nil
}

// scoringHandler implements vmafxv1.VmafxScoringServer for the node. It mirrors
// the standalone vmafx-server handler (cmd/vmafx-server/grpc_server.go) but is
// independent so the node binary does not depend on package main of another
// command. The scoring engine (pkg/libvmaf) is shared.
type scoringHandler struct {
	vmafxv1.UnimplementedVmafxScoringServer
	scorer *libvmaf.Scorer
	log    *slog.Logger
}

func newScoringHandler(scorer *libvmaf.Scorer, log *slog.Logger) *scoringHandler {
	if log == nil {
		log = slog.Default()
	}
	return &scoringHandler{scorer: scorer, log: log}
}

// Health reports node liveness. Always available, even without a scorer.
func (h *scoringHandler) Health(_ context.Context, _ *vmafxv1.HealthRequest) (*vmafxv1.HealthResponse, error) {
	return &vmafxv1.HealthResponse{Ok: true, Message: "ok"}, nil
}

// Score runs the unary file-path scoring path.
func (h *scoringHandler) Score(ctx context.Context, req *vmafxv1.ScoreRequest) (*vmafxv1.ScoreResponse, error) {
	if h.scorer == nil {
		return nil, status.Errorf(codes.FailedPrecondition, "node has no scorer configured")
	}
	if req.GetReference() == "" || req.GetDistorted() == "" {
		return nil, status.Errorf(codes.InvalidArgument, "reference and distorted paths are required")
	}
	score, features, err := h.scorer.Score(ctx, req.GetReference(), req.GetDistorted(), req.GetModel())
	if err != nil {
		h.log.Error("node Score failed", "error", err)
		return nil, status.Errorf(codes.Internal, "scoring failed: %v", err)
	}
	return &vmafxv1.ScoreResponse{Score: score, Features: features}, nil
}

// ScoreStream runs the in-memory per-frame streaming scoring path (ADR-0933).
// It shares the contract and engine with the vmafx-server handler: opening
// StreamConfig, then FramePair messages, then EOF; the server flushes and
// streams back one FrameScore per frame plus a terminal AggregateScore.
func (h *scoringHandler) ScoreStream(stream vmafxv1.VmafxScoring_ScoreStreamServer) error {
	if h.scorer == nil {
		return status.Errorf(codes.FailedPrecondition, "node has no scorer configured")
	}
	ctx := stream.Context()
	start := time.Now()

	first, err := stream.Recv()
	if err != nil {
		return status.Errorf(codes.InvalidArgument, "ScoreStream requires an opening StreamConfig message: %v", err)
	}
	cfg := first.GetConfig()
	if cfg == nil {
		return status.Errorf(codes.InvalidArgument, "ScoreStream: first message must set the `config` oneof, got payload=%T", first.GetPayload())
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
	modelPath, err := h.scorer.ResolveModel(cfg.GetModel())
	if err != nil {
		return status.Errorf(codes.InvalidArgument, "ScoreStream: model %q: %v", cfg.GetModel(), err)
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
		return streamScorerStatus(err)
	}
	defer scorer.Close()

	for {
		if cerr := ctx.Err(); cerr != nil {
			return status.FromContextError(cerr).Err()
		}
		msg, recvErr := stream.Recv()
		if recvErr == io.EOF {
			break
		}
		if recvErr != nil {
			if cerr := ctx.Err(); cerr != nil {
				return status.FromContextError(cerr).Err()
			}
			return status.Errorf(codes.Internal, "ScoreStream: receive frame: %v", recvErr)
		}
		fp := msg.GetFramePair()
		if fp == nil {
			return status.Errorf(codes.InvalidArgument,
				"ScoreStream: post-config message must set the `frame_pair` oneof, got payload=%T", msg.GetPayload())
		}
		if pushErr := scorer.PushFrame(int(fp.GetFrameIndex()), fp.GetRawReference(), fp.GetRawDistorted()); pushErr != nil {
			return streamScorerStatus(pushErr)
		}
	}

	result, err := scorer.Finish(ctx)
	if err != nil {
		return streamScorerStatus(err)
	}

	for _, fr := range result.Frames {
		if cerr := ctx.Err(); cerr != nil {
			return status.FromContextError(cerr).Err()
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
			return sendErr
		}
	}

	elapsed := time.Since(start)
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
		return sendErr
	}

	h.log.Info("node ScoreStream completed",
		"frames", result.FramesProcessed,
		"score", fmt.Sprintf("%.4f", result.Score),
	)
	return nil
}

// protoPixelFormat maps a proto PixelFormat enum to the libvmaf chroma layout
// plus bit depth.
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

// streamScorerStatus maps a pkg/libvmaf typed error to the right gRPC status.
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

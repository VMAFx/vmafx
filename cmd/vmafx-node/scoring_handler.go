// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/scoring_handler.go — the node's VmafxScoring gRPC service
// implementation.
//
// The node exposes the VmafxScoring service (proto/vmafx.proto) so a controller
// — or any gRPC client — can dispatch scoring work directly to the node (push
// model): Score for file-path unary scoring, ScoreStream for in-memory per-frame
// streaming (ADR-0933), and Health for liveness probes. The node binary connects
// libvmaf via cgo (ADR-0713), so the served scoring surface reuses the same
// pkg/libvmaf engine the standalone vmafx-server uses.
//
// Service selection (ADR-0713 / ADR-0709): the worker-side gRPC surface is the
// scoring contract, not a bespoke node API — the only scoring service the proto
// defines is VmafxScoring, and the controller's Node API (RegisterNode /
// PullWork / ReportResult) is a *client* role the node plays against the
// controller, not a service the node hosts. Registering VmafxScoring here gives
// the node a dispatchable scoring endpoint without inventing a second contract.
//
// When the scorer is nil the node still serves Health (so the k8s liveness probe
// and smoke test pass) but returns codes.FailedPrecondition from Score /
// ScoreStream, since scoring requires a libvmaf-backed scorer.
//
// ADR-1119: migrated out of the (now-removed) cmd/vmafx-node/server package into
// package main so the fx composition root can register it directly on the
// golusoris-provided *grpc.Server. The handler logic is unchanged; only its
// host package and constructor signature moved (it now also carries the encoder
// inventory for future capability reporting).

//go:build cgo

package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"time"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	"github.com/VMAFx/vmafx/cmd/vmafx-node/probe"
	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
)

// scoringHandler implements vmafxv1.VmafxScoringServer for the node. It mirrors
// the standalone vmafx-server handler (cmd/vmafx-server/grpc_server.go) but is
// independent so the node binary does not depend on package main of another
// command. The scoring engine (pkg/libvmaf) is shared.
type scoringHandler struct {
	vmafxv1.UnimplementedVmafxScoringServer
	scorer    *libvmaf.Scorer
	inventory *probe.Inventory
	log       *slog.Logger
}

// newScoringHandler builds the node's gRPC scoring service implementation. scorer
// may be nil (Health-only node). inventory is the shared startup-probe result
// (populated in provideEncoderInventory's OnStart hook); it is carried for future
// capability reporting and so a regression that drops the probe wiring is caught
// by the graph.
func newScoringHandler(scorer *libvmaf.Scorer, inventory *probe.Inventory, log *slog.Logger) *scoringHandler {
	if log == nil {
		log = slog.Default()
	}
	if inventory == nil {
		inventory = probe.EmptyInventory()
	}
	return &scoringHandler{scorer: scorer, inventory: inventory, log: log}
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
					FrameIndex: libvmaf.SafeUint32(fr.Index),
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
				FramesProcessed: libvmaf.SafeUint32(result.FramesProcessed),
				Score:           result.Score,
				Features:        result.Features,
				ElapsedMs:       libvmaf.SafeUint64(elapsed.Milliseconds()),
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

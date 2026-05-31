// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/score/grpc_client.go — gRPC client wrapper for vmafx-server.
//
// Provides a small, opinionated wrapper around the generated VmafxScoring
// client that:
//
//   - Hides the oneof-framing boilerplate of the ScoreStream RPC behind a
//     PushFrame / Recv / CloseSend API.
//   - Centralises dial defaults (insecure-by-default for now; ADR follow-up
//     for mTLS).
//   - Returns Score results in their typed form rather than the raw oneof.
//
// ADR-0703: vmafx-server Go gRPC + HTTP service.
// ADR-0933: ScoreStream bidirectional RPC (Phase 1 — schema + stub).

package score

import (
	"context"
	"fmt"
	"io"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"

	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
)

// Client is a thin wrapper around vmafxv1.VmafxScoringClient that exposes
// both the unary v1 surface and the streaming v2 surface in idiomatic Go.
type Client struct {
	conn *grpc.ClientConn
	api  vmafxv1.VmafxScoringClient
}

// Dial returns a new Client connected to addr (e.g. "vmafx-server:50051").
//
// The connection uses insecure transport by default. Production deployments
// should wrap this with TLS credentials (follow-up ADR after Phase 2).
func Dial(addr string) (*Client, error) {
	conn, err := grpc.NewClient(addr, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return nil, fmt.Errorf("score: dial %s: %w", addr, err)
	}
	return &Client{conn: conn, api: vmafxv1.NewVmafxScoringClient(conn)}, nil
}

// Close releases the underlying gRPC connection.
func (c *Client) Close() error {
	return c.conn.Close()
}

// Score calls the unary v1 Score RPC against path-based inputs. Preserved
// unchanged from the original v1 surface — additive streaming is in
// ScoreStream below.
func (c *Client) Score(ctx context.Context, reference, distorted, model string) (float64, map[string]float64, error) {
	resp, err := c.api.Score(ctx, &vmafxv1.ScoreRequest{
		Reference: reference,
		Distorted: distorted,
		Model:     model,
	})
	if err != nil {
		return 0, nil, fmt.Errorf("score: unary Score: %w", err)
	}
	return resp.GetScore(), resp.GetFeatures(), nil
}

// FrameScore is the typed per-frame result emitted on the ScoreStream.
type FrameScore struct {
	Index    uint32
	Score    float64
	Features map[string]float64
}

// Aggregate is the typed terminal result emitted on the ScoreStream after
// the client half-closes.
type Aggregate struct {
	FramesProcessed uint32
	Score           float64
	Features        map[string]float64
	ElapsedMs       uint64
}

// ScoreStream wraps an open bidirectional ScoreStream RPC. The caller drives
// it via PushFrame for each (reference, distorted) frame pair, calls
// CloseSend when no more frames are coming, then drains Recv until the
// terminal Aggregate.
type ScoreStream struct {
	raw vmafxv1.VmafxScoring_ScoreStreamClient
}

// OpenScoreStream opens a new ScoreStream RPC and sends the leading
// StreamConfig message. The returned ScoreStream is ready for PushFrame.
//
// model and frameCountHint may be zero / empty if not known.
func (c *Client) OpenScoreStream(
	ctx context.Context,
	width, height uint32,
	pixelFormat vmafxv1.PixelFormat,
	model string,
	frameCountHint uint32,
) (*ScoreStream, error) {
	raw, err := c.api.ScoreStream(ctx)
	if err != nil {
		return nil, fmt.Errorf("score: open ScoreStream: %w", err)
	}
	cfg := &vmafxv1.ScoreStreamRequest{
		Payload: &vmafxv1.ScoreStreamRequest_Config{
			Config: &vmafxv1.StreamConfig{
				Width:          width,
				Height:         height,
				PixelFormat:    pixelFormat,
				Model:          model,
				FrameCountHint: frameCountHint,
			},
		},
	}
	if err := raw.Send(cfg); err != nil {
		return nil, fmt.Errorf("score: send StreamConfig: %w", err)
	}
	return &ScoreStream{raw: raw}, nil
}

// PushFrame sends one (reference, distorted) frame pair. The raw byte
// buffers must be planar Y/U/V in the pixel format declared by the
// StreamConfig, with no padding between planes.
//
// frameIndex must be strictly monotonically increasing from 0.
func (s *ScoreStream) PushFrame(frameIndex uint32, rawReference, rawDistorted []byte) error {
	msg := &vmafxv1.ScoreStreamRequest{
		Payload: &vmafxv1.ScoreStreamRequest_FramePair{
			FramePair: &vmafxv1.FramePair{
				FrameIndex:   frameIndex,
				RawReference: rawReference,
				RawDistorted: rawDistorted,
			},
		},
	}
	if err := s.raw.Send(msg); err != nil {
		return fmt.Errorf("score: push frame %d: %w", frameIndex, err)
	}
	return nil
}

// CloseSend signals end-of-input. The server will emit any remaining
// FrameScore messages followed by exactly one terminal Aggregate.
func (s *ScoreStream) CloseSend() error {
	if err := s.raw.CloseSend(); err != nil {
		return fmt.Errorf("score: close send: %w", err)
	}
	return nil
}

// Recv returns the next FrameScore or, when the terminal AggregateScore
// arrives, returns a non-nil Aggregate (and a nil FrameScore). After the
// Aggregate is returned, the next Recv returns io.EOF.
//
// The caller's typical loop:
//
//	for {
//	    fs, agg, err := s.Recv()
//	    if errors.Is(err, io.EOF) { break }
//	    if err != nil { return err }
//	    if agg != nil { finalize(agg); continue }
//	    handleFrame(fs)
//	}
func (s *ScoreStream) Recv() (*FrameScore, *Aggregate, error) {
	resp, err := s.raw.Recv()
	if err != nil {
		if err == io.EOF {
			return nil, nil, io.EOF
		}
		return nil, nil, fmt.Errorf("score: recv: %w", err)
	}
	if fs := resp.GetFrameScore(); fs != nil {
		return &FrameScore{
			Index:    fs.GetFrameIndex(),
			Score:    fs.GetScore(),
			Features: fs.GetFeatures(),
		}, nil, nil
	}
	if agg := resp.GetAggregate(); agg != nil {
		return nil, &Aggregate{
			FramesProcessed: agg.GetFramesProcessed(),
			Score:           agg.GetScore(),
			Features:        agg.GetFeatures(),
			ElapsedMs:       agg.GetElapsedMs(),
		}, nil
	}
	return nil, nil, fmt.Errorf("score: recv: response had no payload set")
}

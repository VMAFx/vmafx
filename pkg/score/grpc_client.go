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
	"errors"
	"fmt"
	"io"

	"go.opentelemetry.io/contrib/instrumentation/google.golang.org/grpc/otelgrpc"
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
//
// ADR-0927: the OTel client stats handler injects the W3C traceparent /
// tracestate headers into every outgoing RPC so the server-side span is
// linked as a child of the caller's trace.  No-op when InitOTel installed
// no-op providers (i.e. OTEL_EXPORTER_OTLP_ENDPOINT is unset).
func Dial(addr string) (*Client, error) {
	conn, err := grpc.NewClient(addr,
		grpc.WithTransportCredentials(insecure.NewCredentials()),
		grpc.WithStatsHandler(otelgrpc.NewClientHandler()),
	)
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
//
// Send-error surfacing: gRPC bidirectional streams report a Send failure as
// `io.EOF` when the server has already closed its side with an error (e.g.
// `codes.InvalidArgument` for a malformed StreamConfig). The actual server
// status only surfaces on `Recv`. We therefore convert a Send `io.EOF` into
// the underlying gRPC status by draining `Recv`, so callers get the real
// reason ("InvalidArgument: …") rather than the meaningless `EOF`. ADR-0978.
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
		return nil, fmt.Errorf("score: send StreamConfig: %w", recvStatusOnEOF(raw, err))
	}
	return &ScoreStream{raw: raw}, nil
}

// recvStatusOnEOF translates a stream Send `io.EOF` into the server's actual
// gRPC status. Per the gRPC Go docs, Send returns `io.EOF` whenever the
// stream is "done" from the server's perspective; the caller must then call
// Recv to obtain the underlying status. For any other error we return it
// unchanged (Send may also return network errors that already carry full
// context). When Recv returns nil (no error message available, e.g. server
// closed cleanly), we fall through to the original error so the caller still
// gets a non-nil cause.
func recvStatusOnEOF(s vmafxv1.VmafxScoring_ScoreStreamClient, sendErr error) error {
	if !errors.Is(sendErr, io.EOF) {
		return sendErr
	}
	if _, err := s.Recv(); err != nil && !errors.Is(err, io.EOF) {
		return err
	}
	return sendErr
}

// PushFrame sends one (reference, distorted) frame pair. The raw byte
// buffers must be planar Y/U/V in the pixel format declared by the
// StreamConfig, with no padding between planes.
//
// frameIndex must be strictly monotonically increasing from 0.
//
// On Send failure, this surfaces the server's actual gRPC status (see
// OpenScoreStream's recvStatusOnEOF comment for the rationale). ADR-0978.
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
		return fmt.Errorf("score: push frame %d: %w", frameIndex, recvStatusOnEOF(s.raw, err))
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
		// Defensive: prefer errors.Is over `==` even though the gRPC client
		// surfaces io.EOF as the bare sentinel today. Future gRPC versions
		// may wrap it (e.g. into a status with codes.OK), and this keeps the
		// caller-visible EOF semantics stable. ADR-0978.
		if errors.Is(err, io.EOF) {
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

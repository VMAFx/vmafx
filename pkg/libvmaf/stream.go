// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// pkg/libvmaf/stream.go — in-memory per-frame streaming scoring path
// (ADR-0933 Phase 2).
//
// StreamScorer drives a single, stateful scoring context across a sequence of
// raw planar (ref, dis) frame pairs supplied as in-memory byte buffers rather
// than file paths.  It backs the gRPC ScoreStream bidirectional RPC defined in
// proto/vmafx/v1/vmafx.proto: the server feeds each FramePair's raw_reference /
// raw_distorted bytes into PushFrame, then calls Finish to obtain per-frame and
// pooled scores.
//
// Relationship to direct.go: ScoreDirect (ADR-0931) reads frames from on-disk
// YUV files through an io.Reader.  StreamScorer shares the same session
// (engine.go: context, model, frame fill, submit, flush, pooled score) on the
// generated pkg/vmafx binding (#2434) but sources planes from caller-owned
// []byte and keeps the context alive across calls so a long-lived stream does
// not re-load the model per frame.  This file holds no cgo.
//
// Per-frame VMAF semantics: several VMAF features (notably motion) are temporal
// and are only finalised once the whole sequence has been read and flushed.
// StreamScorer therefore computes per-frame scores in Finish, AFTER the flush,
// via vmafx_score_frame — not incrementally inside PushFrame.  Callers that
// need to emit a FrameScore per frame stream the Finish output back to the
// client after the client half-closes.  This matches the ScoreStreamResponse
// oneof contract (N FrameScore messages followed by one terminal
// AggregateScore) without inventing a lookahead the engine cannot honour.
//
// Goroutine safety: a StreamScorer owns one VmafxContext and is NOT safe for
// concurrent use.  Each gRPC ScoreStream invocation constructs its own
// StreamScorer; the gRPC runtime guarantees a single goroutine per stream
// handler, so no locking is required.  Different streams use different
// StreamScorers and therefore different contexts, which libvmaf supports.
//
// Locale: LC_NUMERIC=C is pinned process-wide by pkg/vmafx (ADR-0137); no
// additional locale handling is needed here.

//go:build cgo

package libvmaf

import (
	"context"
	"fmt"

	"github.com/VMAFx/vmafx/pkg/vmafx"
)

// StreamConfig carries the per-stream geometry and model selection for a
// StreamScorer.  It mirrors the proto StreamConfig message but stays free of
// any gRPC type so pkg/libvmaf has no dependency on the generated stubs.
type StreamConfig struct {
	// Width / Height of every frame in the stream, in luma pixels.
	Width, Height int
	// PixFmt is the chroma subsampling layout shared by ref and dis.
	PixFmt PixelFormat
	// BitDepth is 8, 10, or 12.  10/12 use 2-byte samples (libvmaf bpc).
	BitDepth int
	// ModelPath is the absolute path to the VMAF model JSON.
	ModelPath string
	// PoolMethod selects the temporal pooling strategy. Zero / unset defaults to PoolMethodMean.
	PoolMethod PoolMethod
	// FrameCountHint, when > 0, pre-sizes the per-frame score slice.  Zero
	// means unknown / open-ended and the slice grows on demand.
	FrameCountHint int
}

// FrameResult is the per-frame outcome produced by Finish.
type FrameResult struct {
	// Index is the zero-based frame index (matches the pushed frame order).
	Index int
	// Score is the predicted per-frame VMAF score.
	Score float64
	// Features holds the per-frame per-feature scores keyed by feature name.
	Features map[string]float64
}

// StreamResult is the aggregate outcome produced by Finish.
type StreamResult struct {
	// Frames holds one FrameResult per successfully pushed frame, in order.
	Frames []FrameResult
	// Score is the pooled (mean) VMAF over the whole stream.
	Score float64
	// Features holds the pooled per-feature scores keyed by feature name.
	Features map[string]float64
	// FramesProcessed is len(Frames); duplicated for caller convenience.
	FramesProcessed int
}

// streamFeatures is the fixed set of per-feature scores StreamScorer fetches
// for each frame and for the pooled aggregate.  These are the elementary VMAF
// features the vmaf_v0.6.1-family models register via
// vmaf_use_features_from_model — the exact lookup keys are the model's
// `feature_names` (e.g. "VMAF_integer_feature_adm2_score").
// vmaf_feature_score_at_index returns an error for any feature the loaded model
// did not register, which StreamScorer treats as "not available" and silently
// skips rather than failing the whole frame, so a NEG or 4K model that exposes
// a different feature set still produces per-frame VMAF scores.
var streamFeatures = []string{
	"VMAF_integer_feature_adm2_score",
	"VMAF_integer_feature_motion2_score",
	"VMAF_integer_feature_vif_scale0_score",
	"VMAF_integer_feature_vif_scale1_score",
	"VMAF_integer_feature_vif_scale2_score",
	"VMAF_integer_feature_vif_scale3_score",
}

// StreamScorer holds a live scoring context for in-memory per-frame scoring.
// Construct with NewStreamScorer, feed frames with PushFrame, finalise with
// Finish, and always Close (idempotent) to release the context and model.
type StreamScorer struct {
	cfg             StreamConfig
	vmafCtx         *vmafx.Context
	model           *vmafx.Model
	geom            frameGeometry
	frameSize       int
	nFrames         int
	teardownStarted bool
	owner           cgoScoringOwner
}

// NewStreamScorer initialises a context and loads the model named by
// cfg.ModelPath.  It validates cfg up-front and returns typed errors
// (ErrInvalidArgument / ErrModelNotFound / ...) so the gRPC layer can map them
// to the right status code.  Constructor unwind closes the context before
// destroying its dependent model and makes one bounded close retry.  A
// persistent teardown failure is joined into the returned error; the
// owners remain allocated instead of freeing a still-referenced model.
func NewStreamScorer(cfg StreamConfig) (*StreamScorer, error) {
	const op = "StreamScorer"
	if err := requireGeometry(op, cfg.Width, cfg.Height, cfg.PixFmt, cfg.BitDepth); err != nil {
		return nil, err
	}
	if err := requireModelPath(op, cfg.ModelPath); err != nil {
		return nil, err
	}
	if err := requireModelExists(op, cfg.ModelPath); err != nil {
		return nil, err
	}
	session, err := openScoringSession("NewStreamScorer", cfg.ModelPath, "vmaf_stream")
	if err != nil {
		return nil, err
	}
	geom := newFrameGeometry(cfg.Width, cfg.Height, cfg.PixFmt, cfg.BitDepth)
	return &StreamScorer{
		cfg:       cfg,
		vmafCtx:   session.ctx,
		model:     session.model,
		geom:      geom,
		frameSize: geom.frameSize,
		owner:     session.owner,
	}, nil
}

// session is the scoring session over the scorer's live context and model.
func (s *StreamScorer) session() *scoringSession {
	return &scoringSession{ctx: s.vmafCtx, model: s.model}
}

// FrameSize returns the expected length in bytes of each raw_reference /
// raw_distorted buffer for the configured geometry.  Callers validate the
// proto FramePair payloads against this before calling PushFrame so a malformed
// client frame is rejected with InvalidArgument rather than corrupting the
// picture planes.
func (s *StreamScorer) FrameSize() int { return s.frameSize }

// PushFrame ingests one (ref, dis) raw planar frame pair at index idx.  idx
// must equal the number of frames already pushed (strictly monotonic from 0);
// a gap or repeat is rejected with ErrInvalidArgument.  refBytes / disBytes
// MUST each be exactly FrameSize() bytes of planar Y-U-V data in the configured
// pixel format.  Ownership of the freshly-allocated frames transfers to the
// engine inside vmafx_submit; on any pre-transfer error both frames are
// released so nothing leaks.
func (s *StreamScorer) PushFrame(idx int, refBytes, disBytes []byte) error {
	if s.teardownStarted {
		return fmt.Errorf("StreamScorer: PushFrame after teardown started: %w", ErrInvalidArgument)
	}
	if idx != s.nFrames {
		return fmt.Errorf("StreamScorer: frame index %d out of order (expected %d): %w",
			idx, s.nFrames, ErrInvalidArgument)
	}
	if len(refBytes) != s.frameSize || len(disBytes) != s.frameSize {
		return fmt.Errorf(
			"StreamScorer: frame %d size mismatch (ref=%d dis=%d want=%d): %w",
			idx, len(refBytes), len(disBytes), s.frameSize, ErrInvalidArgument)
	}

	refFrame, disFrame, err := newFramePair(s.geom.desc)
	if err != nil {
		return err
	}
	// vmafx_submit consumes both frames on every path; these Close calls
	// release them only when the submit was never reached.
	defer refFrame.Close()
	defer disFrame.Close()
	if err := fillFrameFromBytes(refFrame, refBytes); err != nil {
		return fmt.Errorf("StreamScorer: copy ref frame %d: %w", idx, err)
	}
	if err := fillFrameFromBytes(disFrame, disBytes); err != nil {
		return fmt.Errorf("StreamScorer: copy dis frame %d: %w", idx, err)
	}
	if err := s.session().submitPair(refFrame, disFrame, idx); err != nil {
		return err
	}
	s.nFrames++
	return nil
}

// Finish flushes the context, fetches the per-frame and pooled scores,
// and returns them.  After Finish the scorer cannot accept more frames; call
// Close to release resources.  Returns ErrPictureRead when zero frames were
// pushed (libvmaf cannot pool an empty interval).
//
// ctx is honoured at frame-iteration boundaries while collecting per-frame
// scores so a client disconnect during the post-flush score harvest aborts
// promptly with the wrapped context error.
func (s *StreamScorer) Finish(ctx context.Context) (*StreamResult, error) {
	if s.teardownStarted {
		return nil, fmt.Errorf("StreamScorer: Finish after teardown started: %w", ErrInvalidArgument)
	}
	if ctx == nil {
		ctx = context.Background()
	}
	if s.nFrames == 0 {
		return nil, fmt.Errorf("StreamScorer: zero frames pushed: %w", ErrPictureRead)
	}

	// Flush: signal end-of-stream so temporal features finalise.
	session := s.session()
	if err := session.flush(); err != nil {
		return nil, err
	}
	frames, err := s.harvestFrames(ctx, session)
	if err != nil {
		return nil, err
	}
	pooled, err := session.pooled(s.cfg.PoolMethod, s.nFrames)
	if err != nil {
		return nil, fmt.Errorf("StreamScorer.Finish: %w", err)
	}
	return &StreamResult{
		Frames:          frames,
		Score:           pooled,
		Features:        s.pooledFeatures(),
		FramesProcessed: s.nFrames,
	}, nil
}

// harvestFrames reads the per-frame score and features of every pushed frame
// after the flush, checking ctx at every frame boundary.
func (s *StreamScorer) harvestFrames(ctx context.Context, session *scoringSession) ([]FrameResult, error) {
	hint := s.cfg.FrameCountHint
	if hint <= 0 {
		hint = s.nFrames
	}
	frames := make([]FrameResult, 0, hint)
	for i := 0; i < s.nFrames; i++ {
		if err := ctx.Err(); err != nil {
			return nil, fmt.Errorf("StreamScorer: cancelled harvesting frame %d: %w", i, err)
		}
		score, err := session.frameScore(i)
		if err != nil {
			return nil, err
		}
		frames = append(frames, FrameResult{
			Index:    i,
			Score:    score,
			Features: s.featuresAtIndex(i),
		})
	}
	return frames, nil
}

// featuresAtIndex collects the per-frame feature scores for frame i.  A feature
// the model did not register returns an error, which is skipped so the
// resulting map contains only features actually available for this model.
func (s *StreamScorer) featuresAtIndex(i int) map[string]float64 {
	out := make(map[string]float64, len(streamFeatures))
	for _, name := range streamFeatures {
		score, err := s.vmafCtx.FeatureScore(name, SafeUint64(int64(i)))
		if err == nil {
			out[name] = score.Value
		}
	}
	return out
}

// pooledFeatures collects the pooled (mean) feature scores over the whole
// stream by averaging each registered feature's per-frame values.  libvmaf has
// no single "pool all features" call, so we reuse the per-frame feature score
// and average in Go — exact for the arithmetic mean the aggregate
// contract specifies.
func (s *StreamScorer) pooledFeatures() map[string]float64 {
	sums := make(map[string]float64, len(streamFeatures))
	counts := make(map[string]int, len(streamFeatures))
	for i := 0; i < s.nFrames; i++ {
		for name, v := range s.featuresAtIndex(i) {
			sums[name] += v
			counts[name]++
		}
	}
	out := make(map[string]float64, len(sums))
	for name, sum := range sums {
		if counts[name] > 0 {
			out[name] = sum / float64(counts[name])
		}
	}
	return out
}

// Close releases the context and then its dependent model.  A failed
// context close retains both owners and may be retried by calling Close again.
// Once teardown starts, PushFrame and Finish reject all further work even when
// the first Close fails.  Successful Close calls are idempotent.
func (s *StreamScorer) Close() error {
	s.teardownStarted = true
	if err := s.owner.Close(); err != nil {
		return fmt.Errorf("StreamScorer.Close: %w", err)
	}
	s.vmafCtx = nil
	s.model = nil
	return nil
}

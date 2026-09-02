// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/libvmaf/stream.go — in-memory per-frame streaming scoring path
// (ADR-0933 Phase 2).
//
// StreamScorer drives a single, stateful libvmaf context across a sequence of
// raw planar (ref, dis) frame pairs supplied as in-memory byte buffers rather
// than file paths.  It backs the gRPC ScoreStream bidirectional RPC defined in
// proto/vmafx.proto: the server feeds each FramePair's raw_reference /
// raw_distorted bytes into PushFrame, then calls Finish to obtain per-frame and
// pooled scores.
//
// Relationship to direct.go: ScoreDirect (ADR-0931) reads frames from on-disk
// YUV files through an io.Reader.  StreamScorer shares the same libvmaf C-call
// sequence (vmaf_init → vmaf_model_load_from_path → vmaf_use_features_from_model
// → per-frame vmaf_picture_alloc ×2 + vmaf_read_pictures → flush →
// vmaf_score_pooled) but sources planes from caller-owned []byte and keeps the
// context alive across calls so a long-lived stream does not re-load the model
// per frame.
//
// Per-frame VMAF semantics: several VMAF features (notably motion) are temporal
// and are only finalised once the whole sequence has been read and flushed.
// StreamScorer therefore computes per-frame scores in Finish, AFTER the flush,
// via vmaf_score_at_index — not incrementally inside PushFrame.  Callers that
// need to emit a FrameScore per frame stream the Finish output back to the
// client after the client half-closes.  This matches the ScoreStreamResponse
// oneof contract (N FrameScore messages followed by one terminal
// AggregateScore) without inventing a lookahead the engine cannot honour.
//
// Goroutine safety: a StreamScorer owns one VmafContext and is NOT safe for
// concurrent use.  Each gRPC ScoreStream invocation constructs its own
// StreamScorer; the gRPC runtime guarantees a single goroutine per stream
// handler, so no locking is required.  Different streams use different
// StreamScorers and therefore different contexts, which libvmaf supports.
//
// Locale: LC_NUMERIC=C is pinned process-wide by direct.go's init() (ADR-0137);
// no additional locale handling is needed here.

//go:build cgo

package libvmaf

/*
#include <libvmaf/libvmaf.h>
#include <libvmaf/model.h>
#include <libvmaf/picture.h>
#include <stdlib.h>
#include <string.h>
*/
import "C"

import (
	"context"
	"fmt"
	"os"
	"unsafe"
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

// StreamScorer holds a live libvmaf context for in-memory per-frame scoring.
// Construct with NewStreamScorer, feed frames with PushFrame, finalise with
// Finish, and always Close (idempotent) to release the context and model.
type StreamScorer struct {
	cfg       StreamConfig
	vmafCtx   *C.VmafContext
	model     *C.VmafModel
	frameSize int
	nFrames   int
	closed    bool
}

// NewStreamScorer initialises a libvmaf context and loads the model named by
// cfg.ModelPath.  It validates cfg up-front and returns typed errors
// (ErrInvalidArgument / ErrModelNotFound / ...) so the gRPC layer can map them
// to the right status code.  On any error the partially-initialised context is
// released before returning, so the caller never needs to Close a failed
// constructor result.
func NewStreamScorer(cfg StreamConfig) (*StreamScorer, error) {
	if cfg.Width <= 0 || cfg.Height <= 0 {
		return nil, fmt.Errorf("StreamScorer: width/height must be positive: %w", ErrInvalidArgument)
	}
	if cfg.PixFmt == 0 {
		return nil, fmt.Errorf("StreamScorer: pix_fmt is required: %w", ErrInvalidArgument)
	}
	if cfg.BitDepth != 8 && cfg.BitDepth != 10 && cfg.BitDepth != 12 {
		return nil, fmt.Errorf("StreamScorer: bit_depth must be 8/10/12, got %d: %w",
			cfg.BitDepth, ErrInvalidArgument)
	}
	if cfg.ModelPath == "" {
		return nil, fmt.Errorf("StreamScorer: model_path is required: %w", ErrInvalidArgument)
	}
	if _, err := os.Stat(cfg.ModelPath); os.IsNotExist(err) {
		return nil, fmt.Errorf("StreamScorer: model %q: %w", cfg.ModelPath, ErrModelNotFound)
	}

	var vmafCtx *C.VmafContext
	vcfg := C.VmafConfiguration{
		log_level:   C.VMAF_LOG_LEVEL_WARNING,
		n_threads:   0,
		n_subsample: 1,
		cpumask:     0,
		gpumask:     0,
	}
	if err := mapErrno("vmaf_init", int(C.vmaf_init(&vmafCtx, vcfg))); err != nil {
		return nil, err
	}

	var model *C.VmafModel
	cModelPath := C.CString(cfg.ModelPath)
	defer C.free(unsafe.Pointer(cModelPath))
	cModelName := C.CString("vmaf_stream")
	defer C.free(unsafe.Pointer(cModelName))
	mcfg := C.VmafModelConfig{
		name:  cModelName,
		flags: C.VMAF_MODEL_FLAGS_DEFAULT,
	}
	if err := mapErrno("vmaf_model_load_from_path",
		int(C.vmaf_model_load_from_path(&model, &mcfg, cModelPath))); err != nil {
		C.vmaf_close(vmafCtx)
		return nil, err
	}

	if err := mapErrno("vmaf_use_features_from_model",
		int(C.vmaf_use_features_from_model(vmafCtx, model))); err != nil {
		C.vmaf_model_destroy(model)
		C.vmaf_close(vmafCtx)
		return nil, err
	}

	return &StreamScorer{
		cfg:       cfg,
		vmafCtx:   vmafCtx,
		model:     model,
		frameSize: frameBytes(cfg.Width, cfg.Height, cfg.PixFmt, cfg.BitDepth),
	}, nil
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
// pixel format.  Ownership of the freshly-allocated pictures transfers to
// libvmaf inside vmaf_read_pictures; on any pre-transfer error both pictures
// are unref'd so no C heap leaks.
func (s *StreamScorer) PushFrame(idx int, refBytes, disBytes []byte) error {
	if s.closed {
		return fmt.Errorf("StreamScorer: PushFrame after Close: %w", ErrInvalidArgument)
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

	pixFmtC := C.enum_VmafPixelFormat(s.cfg.PixFmt)
	bpc := C.uint(s.cfg.BitDepth)
	w := C.uint(s.cfg.Width)
	h := C.uint(s.cfg.Height)

	var refPic, disPic C.VmafPicture
	if rc := C.vmaf_picture_alloc(&refPic, pixFmtC, bpc, w, h); rc != 0 {
		return mapErrno("vmaf_picture_alloc(ref)", int(rc))
	}
	if rc := C.vmaf_picture_alloc(&disPic, pixFmtC, bpc, w, h); rc != 0 {
		C.vmaf_picture_unref(&refPic)
		return mapErrno("vmaf_picture_alloc(dis)", int(rc))
	}

	if err := copyPlanesInto(&refPic, refBytes); err != nil {
		C.vmaf_picture_unref(&refPic)
		C.vmaf_picture_unref(&disPic)
		return fmt.Errorf("StreamScorer: copy ref frame %d: %w", idx, err)
	}
	if err := copyPlanesInto(&disPic, disBytes); err != nil {
		C.vmaf_picture_unref(&refPic)
		C.vmaf_picture_unref(&disPic)
		return fmt.Errorf("StreamScorer: copy dis frame %d: %w", idx, err)
	}

	// Ownership transfers to libvmaf; do NOT unref after this returns 0.
	if err := mapErrno("vmaf_read_pictures",
		int(C.vmaf_read_pictures(s.vmafCtx, &refPic, &disPic, C.uint(idx)))); err != nil {
		return err
	}
	s.nFrames++
	return nil
}

// Finish flushes the libvmaf context, fetches the per-frame and pooled scores,
// and returns them.  After Finish the scorer cannot accept more frames; call
// Close to release resources.  Returns ErrPictureRead when zero frames were
// pushed (libvmaf cannot pool an empty interval).
//
// ctx is honoured at frame-iteration boundaries while collecting per-frame
// scores so a client disconnect during the post-flush score harvest aborts
// promptly with the wrapped context error.
func (s *StreamScorer) Finish(ctx context.Context) (*StreamResult, error) {
	if s.closed {
		return nil, fmt.Errorf("StreamScorer: Finish after Close: %w", ErrInvalidArgument)
	}
	if ctx == nil {
		ctx = context.Background()
	}
	if s.nFrames == 0 {
		return nil, fmt.Errorf("StreamScorer: zero frames pushed: %w", ErrPictureRead)
	}

	// Flush: signal end-of-stream so temporal features finalise.
	if err := mapErrno("vmaf_read_pictures(flush)",
		int(C.vmaf_read_pictures(s.vmafCtx, nil, nil, 0))); err != nil {
		return nil, err
	}

	hint := s.cfg.FrameCountHint
	if hint <= 0 {
		hint = s.nFrames
	}
	frames := make([]FrameResult, 0, hint)
	for i := 0; i < s.nFrames; i++ {
		if err := ctx.Err(); err != nil {
			return nil, fmt.Errorf("StreamScorer: cancelled harvesting frame %d: %w", i, err)
		}
		var score C.double
		if err := mapErrno("vmaf_score_at_index",
			int(C.vmaf_score_at_index(s.vmafCtx, s.model, &score, C.uint(i)))); err != nil {
			return nil, err
		}
		frames = append(frames, FrameResult{
			Index:    i,
			Score:    float64(score),
			Features: s.featuresAtIndex(i),
		})
	}

	var pooled C.double
	if err := mapErrno("vmaf_score_pooled",
		int(C.vmaf_score_pooled(s.vmafCtx, s.model, C.VMAF_POOL_METHOD_MEAN,
			&pooled, 0, C.uint(s.nFrames-1)))); err != nil {
		return nil, err
	}

	return &StreamResult{
		Frames:          frames,
		Score:           float64(pooled),
		Features:        s.pooledFeatures(),
		FramesProcessed: s.nFrames,
	}, nil
}

// featuresAtIndex collects the per-frame feature scores for frame i.  A feature
// the model did not register returns a libvmaf error, which is skipped so the
// resulting map contains only features actually available for this model.
func (s *StreamScorer) featuresAtIndex(i int) map[string]float64 {
	out := make(map[string]float64, len(streamFeatures))
	for _, name := range streamFeatures {
		cName := C.CString(name)
		var fScore C.double
		rc := C.vmaf_feature_score_at_index(s.vmafCtx, cName, &fScore, C.uint(i))
		C.free(unsafe.Pointer(cName))
		if rc == 0 {
			out[name] = float64(fScore)
		}
	}
	return out
}

// pooledFeatures collects the pooled (mean) feature scores over the whole
// stream by averaging each registered feature's per-frame values.  libvmaf has
// no single "pool all features" call, so we reuse vmaf_feature_score_at_index
// per frame and average in Go — exact for the arithmetic mean the aggregate
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

// Close releases the libvmaf context and model.  Idempotent and safe to call
// from a defer even when NewStreamScorer succeeded but Finish was never called
// (e.g. the client disconnected mid-stream).
func (s *StreamScorer) Close() {
	if s.closed {
		return
	}
	s.closed = true
	if s.model != nil {
		C.vmaf_model_destroy(s.model)
		s.model = nil
	}
	if s.vmafCtx != nil {
		C.vmaf_close(s.vmafCtx)
		s.vmafCtx = nil
	}
}

// copyPlanesInto copies tightly-packed planar Y-U-V bytes from src into the
// stride-aligned planes of a freshly-allocated VmafPicture.  The on-wire layout
// is packed (stride == row width); libvmaf's vmaf_picture_alloc may use a wider
// stride, so each row is copied into its stride-aligned slot.
func copyPlanesInto(pic *C.VmafPicture, src []byte) error {
	bytesPerSample := 1
	if pic.bpc > 8 {
		bytesPerSample = 2
	}
	off := 0
	for plane := 0; plane < 3; plane++ {
		w := int(pic.w[plane])
		h := int(pic.h[plane])
		stride := int(pic.stride[plane])
		dataPtr := pic.data[plane]
		if dataPtr == nil || w == 0 || h == 0 {
			continue
		}
		rowBytes := w * bytesPerSample
		dst := unsafe.Slice((*byte)(dataPtr), stride*h)
		for row := 0; row < h; row++ {
			if off+rowBytes > len(src) {
				return fmt.Errorf("plane %d row %d: source exhausted (off=%d need=%d have=%d): %w",
					plane, row, off, rowBytes, len(src), ErrPictureRead)
			}
			copy(dst[row*stride:row*stride+rowBytes], src[off:off+rowBytes])
			off += rowBytes
		}
	}
	if off != len(src) {
		return fmt.Errorf("plane copy consumed %d of %d source bytes: %w",
			off, len(src), ErrPictureRead)
	}
	return nil
}

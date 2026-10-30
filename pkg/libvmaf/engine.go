// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// pkg/libvmaf/engine.go — the in-process scoring session that ScoreDirect and
// StreamScorer share, built on the generated pkg/vmafx binding (#2434).
//
// Both paths used to carry their own cgo copy of the same call sequence
// (vmaf_init, model load, vmaf_use_features_from_model, picture alloc,
// vmaf_read_pictures, flush, vmaf_score_pooled). They now share this file: one
// VmafxContext with its model registered, host frames filled from a reader,
// frames submitted in pairs, and the pooled score read back. Nothing here
// imports "C"; every native call is a method of pkg/vmafx.
//
// Locale: pkg/vmafx pins LC_NUMERIC=C once per process in its init (ADR-0137),
// so no locale handling is needed here.

//go:build cgo

package libvmaf

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"math"
	"os"

	"github.com/VMAFx/vmafx/pkg/vmafx"
)

// maxPlanes is the number of planes of every supported pixel format.
const maxPlanes = 3

// frameGeometry is the per-frame geometry of one scoring run: the frame
// descriptor handed to pkg/vmafx and the on-disk size of one frame.
type frameGeometry struct {
	desc      vmafx.FrameDesc
	frameSize int
}

// newFrameGeometry derives the frame descriptor and the frame size from the
// request geometry.
func newFrameGeometry(width, height int, pixFmt PixelFormat, bitDepth int) frameGeometry {
	return frameGeometry{
		desc: vmafx.FrameDesc{
			PixFmt: SafeUint32(int(pixFmt)),
			Bpc:    SafeUint32(bitDepth),
			W:      SafeUint32(width),
			H:      SafeUint32(height),
		},
		frameSize: frameBytes(width, height, pixFmt, bitDepth),
	}
}

// cgoScoringOwner keeps the model alive until the context close has
// succeeded. A failed context close leaves both callbacks installed so a
// stateful owner can retry without using freed dependencies. Function-scoped
// callers surface the error and deliberately leak the retained objects rather
// than manufacture a use-after-free.
type cgoScoringOwner struct {
	closeContext func() error
	destroyModel func()
}

// mapCloseRC maps the status of a context close: only an exact zero is
// success; a negative errno maps like any libvmaf failure and a positive
// status fails closed.
func mapCloseRC(rc int) error {
	if rc == 0 {
		return nil
	}
	if rc < 0 {
		return mapErrno("vmafx_context_destroy", rc)
	}
	return fmt.Errorf("libvmaf: vmafx_context_destroy returned unexpected positive status %d", rc)
}

func newCGOScoringOwner(ctx *vmafx.Context) cgoScoringOwner {
	return cgoScoringOwner{
		closeContext: func() error { return mapCloseError(ctx.Close()) },
	}
}

func (o *cgoScoringOwner) ownModel(model *vmafx.Model) {
	o.destroyModel = model.Close
}

// Close releases the context first, then the model. On context failure no
// ownership state is cleared and the model is not destroyed, making a later
// call a retry of the same teardown (ADR-1336).
func (o *cgoScoringOwner) Close() error {
	if o.closeContext != nil {
		if err := o.closeContext(); err != nil {
			return err
		}
		o.closeContext = nil
	}
	if o.destroyModel != nil {
		o.destroyModel()
		o.destroyModel = nil
	}
	return nil
}

// closeWithImmediateRetry is for function-scoped owners that cannot return a
// retry handle to their caller. It makes one bounded retry, preserves the
// first error if both attempts fail, and never destroys the model before a
// successful context close.
func (o *cgoScoringOwner) closeWithImmediateRetry() error {
	firstErr := o.Close()
	if firstErr == nil {
		return nil
	}
	if retryErr := o.Close(); retryErr == nil {
		return nil
	}
	return firstErr
}

func closeAfterError(operation string, primary error, owner *cgoScoringOwner) error {
	if closeErr := owner.closeWithImmediateRetry(); closeErr != nil {
		return errors.Join(primary, fmt.Errorf("%s cleanup: %w", operation, closeErr))
	}
	return primary
}

// scoringSession is one VmafxContext with its model registered.
type scoringSession struct {
	ctx   *vmafx.Context
	model *vmafx.Model
	owner cgoScoringOwner
}

// newScoringConfig is the context configuration both in-process scoring entry
// points use: warning-level logging, the engine's own thread auto-selection,
// no subsampling and no CPU/GPU feature masking.
func newScoringConfig() *vmafx.ContextConfig {
	return &vmafx.ContextConfig{LogLevel: vmafx.LogLevelWarning, NSubsample: 1}
}

// loadScoringModel loads the JSON model at path under the informational name.
// The caller owns the returned model and closes it. A path that does not exist
// is ErrModelNotFound (os.ErrNotExist) as before the collapse: vmafx reports a
// missing file as a file error, so the path is checked again after a failed
// load to keep the errno contract of errors.go.
func loadScoringModel(path, name string) (*vmafx.Model, error) {
	model, err := vmafx.LoadModelFile(&vmafx.ModelConfig{Name: name}, path)
	if err != nil {
		if _, statErr := os.Stat(path); os.IsNotExist(statErr) {
			return nil, fmt.Errorf("libvmaf: vmafx_model_load_file: %w [%w]", ErrModelNotFound, err)
		}
		return nil, mapVmafxError("vmafx_model_load_file", err)
	}
	return model, nil
}

// openScoringSession creates a context, loads the model and registers its
// extractors. On failure the context is closed (one bounded retry) before the
// error returns; operation names the caller in the cleanup error.
func openScoringSession(operation, modelPath, modelName string) (*scoringSession, error) {
	ctx, err := vmafx.New(newScoringConfig())
	if err != nil {
		return nil, mapVmafxError("vmafx_context_create", err)
	}
	session := &scoringSession{ctx: ctx, owner: newCGOScoringOwner(ctx)}
	model, err := loadScoringModel(modelPath, modelName)
	if err != nil {
		return nil, closeAfterError(operation, err, &session.owner)
	}
	session.model = model
	session.owner.ownModel(model)
	if err := mapVmafxError("vmafx_context_use_model", ctx.UseModel(model)); err != nil {
		return nil, closeAfterError(operation, err, &session.owner)
	}
	return session, nil
}

// newFramePair allocates a zeroed host frame for the reference and one for the
// distorted picture.
func newFramePair(desc vmafx.FrameDesc) (ref, dis *vmafx.Frame, err error) {
	ref, err = vmafx.NewHostFrame(desc)
	if err != nil {
		return nil, nil, mapVmafxError("vmafx_frame_create_host(ref)", err)
	}
	dis, err = vmafx.NewHostFrame(desc)
	if err != nil {
		ref.Close()
		return nil, nil, mapVmafxError("vmafx_frame_create_host(dis)", err)
	}
	return ref, dis, nil
}

// submitPair hands one reference of each frame to the engine; vmafx_submit
// consumes both on every path, so the frames are closed by the call and a
// later Close is a no-op.
func (s *scoringSession) submitPair(ref, dis *vmafx.Frame, index int) error {
	return mapVmafxError("vmafx_submit", s.ctx.Submit(ref, dis, SafeUint64(int64(index))))
}

// flush signals end-of-stream so the temporal extractors finalise.
func (s *scoringSession) flush() error {
	return mapVmafxError("vmafx_flush", s.ctx.Flush())
}

// pooled pools the model score over frames [0, frames-1].
func (s *scoringSession) pooled(method PoolMethod, frames int) (float64, error) {
	pool, err := method.toPool()
	if err != nil {
		return 0, fmt.Errorf("%w: %v", ErrInvalidArgument, err)
	}
	score, err := s.ctx.ScorePooled(s.model, pool, 0, SafeUint64(int64(frames-1)))
	if err != nil {
		return 0, mapVmafxError("vmafx_score_pooled", err)
	}
	return score.Value, nil
}

// frameScore is the model score of frame index.
func (s *scoringSession) frameScore(index int) (float64, error) {
	score, err := s.ctx.ScoreFrame(s.model, SafeUint64(int64(index)))
	if err != nil {
		return 0, mapVmafxError("vmafx_score_frame", err)
	}
	return score.Value, nil
}

// provenance is the provenance record of the context as JSON, the text the
// CLI report embeds (vmafx_context_provenance_json(), RC4 WP5): one key per
// field of VmafxProvenance and its models, features and annotations.
func (s *scoringSession) provenance() ([]byte, error) {
	text, err := s.ctx.ProvenanceJson(0)
	if err != nil {
		return nil, mapVmafxError("vmafx_context_provenance_json", err)
	}
	return []byte(text), nil
}

// fillFrame reads one frame's worth of raw planar samples from r into the
// planes of frame, row by row, honouring the plane stride. It returns the
// number of bytes read and the first read error unchanged (io.EOF for a clean
// end-of-file before the first byte of a row), so callers can tell the end of
// a stream from a short frame.
func fillFrame(frame *vmafx.Frame, r io.Reader) (int, error) {
	planes, err := frame.Planes()
	if err != nil {
		return 0, mapVmafxError("vmafx_frame_planes", err)
	}
	bytesPerSample := 1
	if planes.Bpc > 8 {
		bytesPerSample = 2
	}
	total := 0
	for plane := 0; plane < maxPlanes && plane < int(planes.NPlanes); plane++ {
		n, err := fillPlane(frame, r, planes, plane, bytesPerSample)
		total += n
		if err != nil {
			return total, err
		}
	}
	return total, nil
}

// fillPlane reads the rows of one plane into the frame.
func fillPlane(frame *vmafx.Frame, r io.Reader, planes vmafx.FramePlanes, plane, bytesPerSample int) (int, error) {
	rows := int(planes.H[plane])
	rowBytes := int(planes.W[plane]) * bytesPerSample
	if rows == 0 || rowBytes == 0 {
		return 0, nil
	}
	if planes.Stride[plane] > math.MaxInt32 {
		return 0, fmt.Errorf("libvmaf: plane %d stride %d is out of range: %w",
			plane, planes.Stride[plane], ErrPictureRead)
	}
	stride := int(planes.Stride[plane])
	dst, err := frame.PlaneBytes(SafeUint32(plane))
	if err != nil {
		return 0, mapVmafxError("vmafx_frame_planes", err)
	}
	if rowBytes > stride || len(dst) < (rows-1)*stride+rowBytes {
		return 0, fmt.Errorf("libvmaf: plane %d layout (%d rows of %d bytes, stride %d, %d bytes): %w",
			plane, rows, rowBytes, stride, len(dst), ErrPictureRead)
	}
	total := 0
	for row := 0; row < rows; row++ {
		n, err := io.ReadFull(r, dst[row*stride:row*stride+rowBytes])
		total += n
		if err != nil {
			return total, err
		}
	}
	return total, nil
}

// fillFrameFromBytes copies a tightly packed planar frame from src into the
// planes of frame. A source that is too short or too long is ErrPictureRead.
func fillFrameFromBytes(frame *vmafx.Frame, src []byte) error {
	reader := bytes.NewReader(src)
	n, err := fillFrame(frame, reader)
	if err != nil {
		if errors.Is(err, io.EOF) || errors.Is(err, io.ErrUnexpectedEOF) {
			return fmt.Errorf("source exhausted after %d of %d bytes: %w", n, len(src), ErrPictureRead)
		}
		return err
	}
	if reader.Len() != 0 {
		return fmt.Errorf("plane copy consumed %d of %d source bytes: %w", n, len(src), ErrPictureRead)
	}
	return nil
}
